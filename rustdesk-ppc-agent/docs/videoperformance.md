# Video performance on the G5

Everything here was measured on the target machine — a dual PowerPC 970 @ 2.3 GHz,
4 GB, Mac OS X 10.5.8, running at 1920x1080. Numbers are from `--probe-display`
and the C probes in `probes/`.

**The short version:** capture is a raw VRAM read, and no codec setting touches
it. The only real optimisation is reading fewer bytes. That took a capture cycle
from 6364 ms to 14 ms on an idle desktop.

Three corrections worth reading before trusting older numbers here. The idle
figure was once quoted as 6 ms, measured while the dirty-band probe was reading
one byte per sampled pixel -- which landed on the constant alpha byte, so it was
timing a probe that could not detect anything. For a while the agent ran a
display capture/release cycle before each probe, which added ~256 ms to *every*
poll; that turned out to be unnecessary as well as hostile to the machine's own
user, and is gone. And the probe then spent a while hashing the framebuffer
directly, at 1.9 MB/s, before anyone checked it against the 22.8 MB/s a bulk
copy gets. It now samples a quarter of every eighth row, `memcpy`s those runs
into RAM, hashes them there, and costs **14 ms**. See `src/capture.rs`.

---

## 1. Never read the framebuffer per pixel

The first working converter read pixels straight out of the framebuffer and took
**6364 ms per frame** — 0.16 fps. The framebuffer is uncached VRAM, so every
scattered read is a bus transaction:

| operation (7.9 MB frame) | time |
|---|---|
| VRAM → RAM `memcpy` | 347 ms (22.8 MB/s) |
| ARGB→Y **from RAM** | 14 ms |
| ARGB→Y **from VRAM** | **3070 ms** |

**220x.** Bulk transfer is tolerable; per-pixel access is not.

`Capturer::frame()` therefore does one `memcpy` into a RAM shadow and converts
from that. On its own that took the cycle from 6364 ms to 543 ms.

> Anything touching the framebuffer must be a bulk copy. Convert, hash, diff,
> encode — all of it happens against the RAM shadow.

## 2. Read cost is proportional to bytes, with no striding penalty

| read | time |
|---|---|
| full frame | 348.6 ms |
| every 2nd row | 166.1 ms |
| every 4th row | 82.9 ms |
| every 8th row | 41.5 ms |
| every 16th row | 20.9 ms |
| every 32nd row | 10.4 ms |
| top 1/2 contiguous | 165.6 ms |
| top 1/4 contiguous | 83.5 ms |
| top 1/8 contiguous | 41.4 ms |
| top 1/16 contiguous | 20.5 ms |

Strided and contiguous reads of the same volume cost the same. That is what
makes partial capture worthwhile: we can read exactly the rows we need and pay
exactly in proportion.

Resolution scales the same way — all of these land at ~25 MB/s:

| resolution | bytes | capture |
|---|---|---|
| 1920x1080 | 7.9 MB | 348 ms |
| 1024x768 | 3.1 MB | 126 ms |
| 800x600 | 1.9 MB | 77 ms |
| 640x480 | 1.2 MB | 49 ms |

**Lowering the G5's own display resolution is the single largest remaining
win**, and it costs no code.

## 3. Dirty-band detection

A desktop is mostly static, so the real waste was paying 348 ms to discover that
nothing had changed.

`Capturer::dirty_bands()` splits the screen into 16 horizontal bands and
checksums a sample of each — 128 bytes out of every 512 along every 8th row —
then compares against the previous probe. `read_bands()` copies only the bands
that moved.

The sampled runs are `memcpy`'d into RAM and hashed there, which is not a
detail. Hashing them in place reads uncached VRAM a byte at a time, and that
measures 1.9 MB/s against the 22.8 MB/s of a bulk copy — the same 12x mistake as
§1, one level down. Once the copy is bulk, the probe is purely proportional to
bytes sampled: measured on the G5, window sizes from 3 to 7680 bytes all land at
~19 MB/s, so the window is a coverage dial and nothing else.

```text
probe : 14 ms first (16 bands dirty), 14 ms second (0 dirty)
```

**14 ms** for a quarter of every sampled row, and it correctly reports nothing
dirty on a still screen.

### Resulting cost model (1920x1080)

Full pipeline, measured with `--probe-display`:

| stage | cost |
|---|---|
| dirty-band probe | 14 ms |
| capture (VRAM → RAM) | 351 ms |
| ARGB → I420 | 185 ms |
| VP8 encode | 34 ms |
| **full-screen change** | **571 ms** |
| **idle** | **14 ms** |

| scenario | cost | notes |
|---|---|---|
| idle | **14 ms** | probe only, and not on every pass once the screen settles |
| one band changed (typing, a menu) | ~81 ms | probe + band read + convert + encode |
| whole screen changed (video) | ~571 ms | everything |

Note the shape: **capture dominates everything**, at 351 ms against 185 ms for
conversion and 34 ms for the encoder.

For *small* updates the ranking inverts. Capture and conversion are per-band, so
a few changed lines cost tens of milliseconds each, while VP8 still encodes a
whole frame every time -- which makes the encoder the floor for ordinary
interaction even at 34 ms. See [`performance-plan.md`](performance-plan.md) for
what to do about it.

Normal interactive work is genuinely usable; only full-screen motion falls back
to ~2 fps, which is the honest ceiling for this hardware.

### The limitation, stated plainly

The checksum is **sampled, not exhaustive**. A change confined entirely to
unsampled pixels is missed until something else in that band moves. This is the
right trade when the alternative is reading everything every time, but it is a
real gap, not a free lunch. `invalidate()` forces a full frame when a peer
connects, and `session`'s settle repaint bounds how long a missed change can
survive to about a second.

If it proves too lossy in practice, the knobs are `PROBE_ROW_STEP` (currently 8)
and `PROBE_WINDOW` / `PROBE_WINDOW_STEP` (currently 128 in 512) in
`src/capture.rs` — all three trade probe cost for coverage, linearly.
`--probe-display` prints the whole trade as a table.

## 4. Why compression is not the lever

Two independent reasons:

**The bottleneck is upstream of the codec.** 351 of the 571 ms is a raw memory
read that happens before a single pixel is encoded. No encoder setting affects
it.

**There is no raw path to fall back to.** `VideoFrame` at this vintage offers
`VP9s`, `RGB` and `YUV`, which looks like an escape hatch — but `RGB` and `YUV`
are metadata-only markers (`message RGB { bool compress = 1; }`), with pixel data
sent "directly in binary" for the old web client, and **modern RustDesk's client
references neither**. VP8 via libvpx is realistically the only thing a real peer
decodes.

Codec settings still matter for the *encode* step — VP8 rather than VP9,
realtime deadline, `cpu_used = -16`, two threads, a static-skip threshold of
15000, 1500 kbps — and together they have taken it from 92 ms to 34 ms. But that
step measures 34 ms against 536 ms for capture plus conversion. It is additive
to capture, not a substitute for fixing it, and it is the smallest term by a
long way. Where it *does* decide things is small updates, where capture and
conversion are per-band and the encoder is not: see
[`performance-plan.md`](performance-plan.md).

## 5. Conversion cost

`argb_to_i420` takes ~185 ms at 1920x1080. For comparison, hand-written C at
`-O2` did luma-only from RAM in **14 ms**. Chroma explains part of the gap; most
of the rest is that mrustc emits C compiled at **`-O1`**, plus Rust bounds checks
in the hot loop.

Untried, in rough order of expected value:

1. Move the converter into a C shim, as `vpx_shim.c` and `input_shim.c` already
   are. That gets -O2 and no bounds checks in one step, without writing a line
   of vector code, and the build already passes `-maltivec`.
2. `get_unchecked` in the inner loop — the cheaper half of the same idea, if the
   converter is to stay in Rust.
3. Raise the C optimisation level. `ppc-cc-remote.py` passes mrustc's `-O1`
   through and has no override; it strips `OPT_FLAGS` only for oversized units.
4. An AltiVec converter. The G5 has AltiVec and this is exactly the shape of
   problem it suits, but mrustc-generated C will not auto-vectorise — which is
   another reason to do 1 first and measure what is left.

One thing already tried and **not** worth it: fusing the luma and chroma passes
into a single pass over 2x2 blocks, so each pixel is read once instead of twice.
Measured 193 ms vs 196 ms — no meaningful difference, because reads from the RAM
shadow are cheap and the cost is arithmetic. The fused version was kept for being
simpler, not faster.

## 6. Reproducing this

```bash
# on the build host
rustdesk-ppc-agent/probes/run.sh          # builds and runs the C probes on the G5

# on the G5, from the agent itself
~/rustdesk-agent --probe-display
```

### A measurement trap worth knowing

The first attempt at the striding table produced `0.0 ms` for copying megabytes.
gcc at `-O2` had eliminated the `memcpy`s because the destination was never read.
Every probe here now accumulates into a `volatile` sink. If a number looks too
good, check that the result is actually observed.
