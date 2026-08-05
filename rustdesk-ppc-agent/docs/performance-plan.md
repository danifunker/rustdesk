# Making it faster: where the time goes, and what to do about it

Written to be worked through in a later session. Everything labelled *measured*
comes from the machine; everything labelled *estimate* does not, and should be
measured before anyone spends a day on it.

Companion to [`videoperformance.md`](videoperformance.md), which covers how the
capture path got to where it is. This document is about what to do next.

Target: `PowerMac11,2`, **two** PowerPC 970 cores @ 2.3 GHz, 4 GB, Mac OS X
10.5.8, 1920x1080.

---

## 1. The baseline, measured

`rustdesk-agent --probe-display`, 1920x1080, after the probe and encoder work of
2026-08-05:

| stage | cost | this morning | scales with |
|---|---|---|---|
| dirty-band probe | **13 ms** | 29 ms | bytes sampled |
| capture (VRAM → RAM) | 357 ms | 350 ms | **bytes read** |
| ARGB → I420 | **20 ms** | 175 ms | rows converted |
| VP8 encode | **30 ms** | 46 ms | frame size, threads |
| **full-screen change** | **407 ms** | 591 ms | |
| **idle poll** | **13 ms** | 29 ms | |

Capture and conversion are per-band, so a typical small update — a few lines of
text, a menu, a cursor-sized region — costs roughly:

```
probe 14 + read 22 + convert 11 + encode 34  ≈  81 ms       (estimate: band
                                                             costs derived from
                                                             the measured
                                                             per-row scaling)
```

against ~127 ms before. §1a has what a real session measures, which is the
number to trust. **Capture is now the whole story at both scales** — nothing so
far reads fewer bytes out of VRAM, and everything that was competing with it has
been dealt with.

## 1a. What a live session actually costs

`--probe-display` measures stages in isolation. The agent now also logs where
each frame's milliseconds went under a real client, which is the only place some
of this shows up at all (`session::FrameTimes`, at debug level):

```text
frame: 16 band(s), probe 28, read 333, conv 14, enc  27,       send 0, other 0 = 405 ms,   527 B
frame: 16 band(s), probe 37, read 339, conv 14, enc 148 (KEY), send 1, other 0 = 542 ms, 81232 B
frame:  2 band(s), probe 14, read  40, conv  1, enc  23,       send 0, other 0 =  80 ms,   608 B
frame:  1 band(s), probe 20, read  29, conv  0, enc  36,       send 2, other 0 =  90 ms,  4021 B
```

Live-measured progress over one session of work: **a full-screen change went
1208 → 405 ms, a small update 179 → 80-105 ms.** The keyframe is now only the
settle repaint, which runs when nothing is waiting on it.

Three things came out of it that no isolated probe would have found.

**`send` is 0-4 ms.** The network is not a factor, even for a 95 KB frame on
this LAN. That was worth ruling out before optimising anything else.

**`other` was 481 ms of a 1208 ms frame, and is now 0.** `drain_input` blocks
for `POLL_MS` when the socket is empty, and it ran after every band -- so a
sixteen-band frame paid sixteen 30 ms timeouts to discover the peer had said
nothing. Servicing input between bands was right; paying a blocking wait for it
was not. It now peeks first (`session::input_waiting`). **Full-screen 1208 →
749 ms, a small update 179 → 109 ms**, measured before and after against the
same forced repaint.

**Every full-screen frame was a keyframe**, which made the worst case for
capture the worst case for the encoder too. Fixed -- see §4a.

What is left is almost entirely the VRAM read: **333 of a 405 ms full-screen
frame, and ~35 of an 85 ms small update.** Everything else is now small.

## 1b. What a terminal actually needs, which is not throughput

Everything above optimises *frames per second*. A shell session turned out to be
limited by two other things entirely, both found by a person using it and
neither visible in any timing.

**The repair was worse than what it repaired.** The settle repaint re-read all
sixteen bands whenever the screen had been still for 900 ms — and ordinary
typing pauses for longer than that constantly, so a real session hit a
full-screen repaint every ~1.4 seconds, each freezing the picture for 0.6-1.5 s
while the typing itself cost 110 ms. It now repairs a slice at a time,
rotating, so a lap of the screen costs the same but arrives in ~45 ms pieces.

**Sampling coverage is the wrong metric; the gap is the metric.** The probe was
changed to sample a quarter of every row instead of a sixteenth — four times the
bytes — and got *worse* at noticing typing, because clustering those bytes into
128-byte windows leaves a 96-pixel hole between them and thirteen characters fit
in one. Reported as "writing left to right, sometimes a bunch of delay where we
don't detect the screen updates". What matters is that no gap is wider than the
thing being looked for: a terminal glyph is ~7 pixels, so the window is now 8
bytes in 32, a 6-pixel gap, and a character cannot fall in it.

Narrower things still can — a vi insert cursor is ~2 pixels — so sampled rows
now take turns looking at different columns. A glyph is ~12 pixels tall and rows
are sampled every 8, so it always spans two sampled rows, and those two rows
examine different quarters of the width. Twice the effective coverage for no
extra reading.

**And the encoder was dropping parts of letters.** `VP8E_SET_STATIC_THRESHOLD`
at 15000 measured 18 ms faster per frame, and skips any macroblock whose error
falls below it — which includes the one holding the bottom third of a line of
text, because it is mostly blank background. Reported as characters arriving in
halves, "the horizontal part of the t but not the bottom part". Back to 1000.
The reasoning that picked 15000 was that text is high-contrast and nowhere near
the threshold: true of a whole glyph, false of the fraction of one that shares a
macroblock with blank paper.

The lesson worth keeping: **every one of these was a correctness bug that
measured as a speed win.** `--probe-display` reported each change as an
improvement, and a person looking at a terminal reported all three as
regressions.

### A correction to the old figures

The 65 ms once quoted for encode was measured on the *first* inter frame after a
keyframe, which is not what a steady session pays. With three warm-up frames
first, the same settings measure 46 ms. So the encoder was never as bad as
recorded, and the gap between it and capture was always wider than it looked.
`--probe-display` now warms up before timing anything.

## 2. Why VNC feels faster

Worth stating plainly, because it is structural rather than a tuning gap.

A VNC server sends **raw or lightly-compressed rectangles**: for a changed
region it copies those pixels and ships them, with no colour-space conversion
and no codec. Cost is proportional to the changed area and nothing else.

This agent sends **VP8 video frames**, because that is what the RustDesk client
decodes — `VideoFrame` carries a codec bitstream for the whole display, and the
protocol has no "here is a rectangle of pixels" message. So every update pays:

* ARGB → I420 conversion for the rows that changed, and
* a whole-frame VP8 encode regardless.

Anything that closes the gap has to make those cheaper, shrink what is encoded,
or use the second processor. Switching to raw rectangles is not available
without breaking protocol compatibility.

## 3. The second CPU, where there is one

**Ask, do not assume.** Single-processor G4s and G5s are as much a target as the
dual G5 this is developed on, so anything that spends another core has to check
at runtime. `sys::cpu_count` reads `sysconf(_SC_NPROCESSORS_ONLN)` and
`sys::threads_for(cap)` never returns more than the processors present; a
single-processor machine always gets 1.

**Done: threaded encode.** `vpxenc_new` takes a thread count, and VP8 needs
token partitions to match or `g_threads` has nothing to divide along.
*Measured* on the dual G5: **92 ms → 65 ms**, −29%. The agent logs what it chose:

```text
encoder: 1920x1080, 1500 kbps, 2 thread(s) of 2 processor(s), Tune { .. }
```

Two more ways to spend a second core, in increasing order of effort:

1. **Split the conversion across both cores.** `argb_to_i420_rows` already takes
   a row range, so two threads converting half the rows each is a small change
   with no shared state. *Estimate*: 185 ms → ~100 ms full-frame. Must go
   through `sys::threads_for`, and must stay correct on one core.
2. **Pipeline capture against encode.** Capture is a VRAM read that barely
   touches the CPU (~23 MB/s, bus-bound); encode is pure CPU. Reading frame N+1
   while encoding frame N should hide most of one behind the other. Worth less
   than it was now that encode is 34 ms rather than 65 against a 351 ms capture
   — the thing there is to hide is small. It needs a second thread and a buffer
   handoff, and the input loop must keep being serviced throughout.

## 4. Ranked options

| # | change | expected | effort | risk |
|---|---|---|---|---|
| ~~1~~ | ~~Threaded encode~~ | **done: −29% encode** | trivial | none |
| ~~2~~ | ~~Encoder tuning~~ | **done: 46 → 30 ms** | trivial | a quality dial, §5 |
| ~~3~~ | ~~Cheaper probe~~ | **done: 29 → 13 ms, 4x the coverage** | small | none |
| ~~4a~~ | ~~Keyframe on demand, not on volume~~ | **done: −120 ms and −80 KB per full-screen frame** | small | drift, handled by the settle keyframe |
| ~~4b~~ | ~~Non-blocking input drain~~ | **done: −481 ms of a 1208 ms frame** | small | none |
| ~~5a~~ | ~~Converter in C~~ | **done: 199 → 20 ms, planes verified identical** | small | none |
| 1 | **Serve at half resolution** | *est.* 405 → ~210 ms, 85 → ~40 ms | medium | halves sharpness |
| 2 | Threaded band reads | *est.* −0, see below | — | — |
| 3 | Cheaper probe again | 13 → ~7 ms, a sixth of a small update | trivial | less coverage |
| 4 | Pipeline capture against encode | *est.* −30 ms of 405 | large | needs care around input latency |
| 5 | AltiVec conversion | 20 → ~10 ms | medium | not worth it now |
| 6 | Tiles as displays | small updates near-free | large | **speculative, read the client first** |

**The ranking has collapsed to one item.** After today, a full-screen frame is
405 ms of which 333 is the VRAM read, and a small update is 85 ms of which ~35
is the VRAM read. Conversion is 14 ms, the encoder 27, the probe 13. Threading
the conversion (old option 6) would now save 7 ms; pipelining capture against
encode (option 4) would hide 27 ms behind 333. Neither is worth the change.

Reading fewer bytes is the only lever left, and at a fixed 23 MB/s that means
**fewer pixels** — which is option 1 and nothing else.

### 4a — stop forcing a keyframe on every full-screen change

Cheapest item on this list, and it was found by watching a live session rather
than by reasoning. `message_loop` computes `all = dirty.iter().all(|d| *d)` and
passes it as `force_key`, so **whenever all sixteen bands are dirty the agent
forces a keyframe** -- which is to say, every time you drag a window or scroll.
The worst case for capture is thereby also made the worst case for the encoder.
Measured cost of that choice:

```text
16 bands, KEY   : enc 170-181 ms, 95 KB on the wire
 1-2 bands, inter: enc  27- 78 ms,  1-6 KB
```

Nothing appears to need it. The settle repaint repairs missed changes by
re-reading VRAM, and an inter frame carries the corrected pixels just as well; a
new peer gets a keyframe anyway, because a fresh encoder always emits one; and
`kf_mode` is already `VPX_KF_AUTO`, so libvpx inserts them when they are
actually worth it. *Estimate*: −130 ms and −90 KB on every full-screen frame.

The one argument for keeping it is resynchronisation -- a keyframe lets a client
that somehow lost state recover. Worth deciding deliberately rather than by
accident, which is how it is decided now.

### 4 — serve at half resolution

The largest remaining win that needs nothing from the client. Do not change the
G5's own display mode: downscale in the agent and tell the peer the display *is*
960x540, scaling injected mouse coordinates and reported cursor positions by
two. Every stage divides — capture reads only even rows (351 → ~166 ms, straight
off the striding table in `videoperformance.md`), conversion ~185 → ~50 ms,
encode 34 → ~12 ms.

The property that matters: because `PeerInfo` reports the smaller size, the
canvas, the pointer and the video all agree, and no client behaviour has to be
verified. The variant where a small frame is sent into a full-size canvas does
depend on the client, and is option 9's problem.

Coordinate scaling lands in two places — `input::Injector::decide_mouse` and
`cursor::Tracker` — both of which are pure and host-testable. Worth a flag
rather than a decision, since it genuinely costs sharpness on text.

### 5 — converter in C

Ranked above AltiVec deliberately, because the first step is much cheaper than
the second. mrustc emits C compiled at **-O1**, with Rust bounds checks in the
hot loop; hand-written C at -O2 did luma-only from RAM in 14 ms against the
185 ms the whole conversion costs now. `vpx_shim.c` and `input_shim.c` already
establish the pattern, and the build already passes `-maltivec`. Get the plain-C
version first and measure before writing any vector code.

### The alternative capture path, now closed

`probes/winlist-capture.c` finally timed `CGWindowListCreateImage` as a capture
route, full-screen and per-band, against the `memcpy` it would replace. It
**returns NULL** in this context — the agent and the probes both run outside the
Aqua session, and while `CGDisplayBaseAddress` works there, compositing does
not. So there is no faster readback available and the 23 MB/s is the floor:

```text
region           memcpy       MB/s    winlist       MB/s
full screen     338.4 ms       23.4        -             -   (returned nothing)
one band         20.8 ms       23.9        -             -   (returned nothing)
```

It might work from a process genuinely inside the console session, via the
LaunchAgent. That is a large change to how the agent is run, for a route that
composites the whole screen and so cannot serve per-band reads anyway. Recorded
as closed rather than promising.

### 9 — where "encode a sub-rectangle" really stands

There are two blockers, not one. **VP8 keyframes carry the frame dimensions and
inter frames must match them**, so a rectangle that varies per frame would force
a keyframe every frame — expensive and large. And `VideoFrame` has no x/y
placement.

The one protocol-legal route is a *fixed* grid of tiles declared as separate
displays in `PeerInfo`, each with its own x/y offset and its own encoder, with
only the tiles that moved sent. That is the shape modern RustDesk's combined
multi-monitor view composites. It is the only option here that removes the
whole-screen tax and genuinely matches VNC's cost model — and it depends on
client behaviour that must be read in the client's source before any of it is
written, needs N encoders, and may require the user to pick a display mode.

## 5. Already ruled out

Do not re-litigate these; each cost real time to establish.

* **`CGDisplayCapture` to "speed up" or "refresh" capture.** It is unnecessary --
  the mapping is live -- and actively harmful: it seizes the display, registers
  the agent as an application, steals focus and dismisses menus. See
  `src/capture.rs`.
* **Reading the framebuffer per pixel.** 6364 ms per frame against 350 ms for a
  bulk copy. The read must be one `memcpy` per band.
* **Hashing the framebuffer in place.** The same mistake one level down, and it
  survived in the probe until 2026-08-05: a byte-at-a-time hash over uncached
  VRAM measured **1.9 MB/s** against 22.8 MB/s for a bulk copy of the same
  bytes. `memcpy` the sampled runs into RAM and hash *that*. See `PROBE_WINDOW`.
* **Wider probe windows to cut transaction count.** The theory was that the
  probe was latency-bound and that reading 128 bytes cost what 3 bytes cost.
  Measured flatly false: every window size from 3 to 7680 bytes lands at the
  same MB/s, and time tracks volume exactly. The window is a coverage dial, not
  a cost dial.
* **`VP8_EFLAG_NO_REF_GF | NO_REF_ARF`.** The argument was that VP8 searches
  three references per macroblock where one would do. Measured 47 ms against 46:
  nothing. At `cpu_used = -16` the fast mode picker was evidently not searching
  them anyway.
* **Turning off `g_error_resilient`.** Pointless over TCP, so it should have been
  free quality. Measured no faster *and* 9% more bytes per frame. Left on.
* **libyuv's `ARGBToI420`.** Its `ARGB` means little-endian word order, i.e.
  B,G,R,A in memory; this framebuffer is genuinely A,R,G,B. It would silently
  swap red and blue.
* **A faster `cpu_used`.** Already at the fastest the codec accepts, with the
  realtime deadline and no lookahead.
* **Assuming two processors.** The target family includes single-processor
  machines; use `sys::threads_for` and check what it returns on the machine in
  front of you.
* **Measuring a session from the outside.** Before `FrameTimes` existed, the
  only signal was the gap between sends -- which cannot tell work from a screen
  that was not changing, and which hid 481 ms of blocking socket reads inside
  what looked like encode cost. Two of the three findings in §1a were invisible
  to `--probe-display` by construction. If a live session is behaving unlike the
  model, instrument the session rather than re-running the probe.

## 6. How to measure

```bash
export SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock      # the key is passphrase-protected
ssh ppctiger '~/rustdesk-agent --probe-display'   # per-stage timings and both sweeps
PPC_HOST=ppctiger probes/run.sh vram-vs-ram read-scaling
cargo run --example probe_client -- 192.168.99.116:21118 ppctest123
```

`--probe-display` carries two sweeps, so the constants they feed are re-checked
on whatever machine is in front of you rather than inherited from this one:

* **probe shape** — cost against coverage for each `(window, step)`. The MB/s
  column is the check that the sampled runs are being copied at bulk speed;
  anything near 2 MB/s means something is reading VRAM a byte at a time again.
* **vp8 tuning** — each `encode::Tune` timed against a still frame and a small
  change, with the configuration actually in use marked. Three warm-up frames
  first, and the two columns interleaved: run in sequence, a still frame came
  out dearer than a changed one, which is not a thing that can be true.

Three cautions learned the hard way:

* **Do not measure with a client connected.** The agent serves one peer at a
  time and the encode path is shared; `probe_client` will simply fail.
* **Beware measuring an idle desktop.** Frame-rate figures from a screen that is
  not changing measure nothing. Force a change, and know what change you forced
  -- `killall Dock` is throttled by launchd to one respawn per ten seconds,
  which has already produced one wrong conclusion in this project.
* **Warm the encoder up.** See the correction in §1. Anything timed on the first
  frame or two after a keyframe is measuring the keyframe's aftermath.
