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

`rustdesk-agent --probe-display`, 1920x1080, after the change-detection and
per-band work of 2026-08-05:

| stage | cost | scales with |
|---|---|---|
| dirty-band probe | 29 ms | pixels sampled |
| capture (VRAM → RAM) | 350 ms | **bytes read** |
| ARGB → I420 | 175 ms | rows converted |
| VP8 encode | 92 ms | frame size |
| **full-screen change** | **617 ms** | |
| **idle poll** | **29 ms** | |

Capture and conversion are now per-band, so a typical small update — a few lines
of text, a menu, a cursor-sized region — costs roughly:

```
probe 29 + read 22 + convert 11 + encode 92  ≈  154 ms       (estimate,
                                                              band costs derived
                                                              from the measured
                                                              per-row scaling)
```

**That makes the encoder the floor for ordinary interaction, not the capture.**
It is the one stage that still pays full price for a small change, because VP8
encodes a whole frame however little of it moved. The old advice in
`videoperformance.md` — "encode is the cheapest stage, tuning the codec is the
least valuable thing available" — was true when conversion was full-frame and is
no longer true for small updates. Full-screen motion is still capture-bound.

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

Anything that closes the gap has to either make those two cheaper or use the
second processor. Switching to raw rectangles is not available without breaking
protocol compatibility.

## 3. The second CPU is idle

The clearest opportunity, and the first thing to try.

The machine has two cores. The agent is single-threaded end to end, and
`vpx_shim.c` sets `cfg.g_threads = 1`. So one core does capture, conversion and
encoding in series while the other does nothing.

Three ways to spend it, in increasing order of effort:

1. **`g_threads = 2` in the encoder.** One line. libvpx's VP8 encoder threads
   across token partitions, so this may or may not help at this resolution --
   *estimate*: somewhere between nothing and a 40% cut in the 92 ms. Measure
   with `--probe-display` before and after; it is the cheapest experiment here.
2. **Split the conversion across both cores.** `argb_to_i420_rows` already takes
   a row range, so two threads converting half the rows each is a small change
   with no shared state. *Estimate*: 175 ms → ~95 ms full-frame.
3. **Pipeline capture against encode.** Capture is a VRAM read that barely
   touches the CPU (~23 MB/s, bus-bound); encode is pure CPU. Reading frame N+1
   while encoding frame N should hide most of one behind the other. This is the
   biggest win available and the largest change -- it needs a second thread and
   a buffer handoff, and the input loop must keep being serviced throughout.

## 4. Ranked options

| # | change | expected | effort | risk |
|---|---|---|---|---|
| 1 | `g_threads = 2` | *est.* up to −40% encode | trivial | none |
| 2 | Threaded ARGB → I420 | *est.* −80 ms full-frame | small | low |
| 3 | Lower the resolution | capture ∝ pixels: 1024x768 is ~126 ms *measured* | none, it is a setting | changes what the user sees |
| 4 | Pipeline capture against encode | *est.* −40% wall clock | large | needs care around input latency |
| 5 | AltiVec conversion | *est.* 175 ms → ~40 ms | medium | AltiVec on `gcc10`, 4-pixel lanes |
| 6 | Encode a sub-rectangle | would make small updates ~free | large | **check the client accepts it first** |
| 7 | Adaptive probe cadence | −29 ms per idle poll | small | slower to notice a change |

Option 6 is the one that would actually close the gap with VNC, and it is also
the one most likely to be impossible: it depends on whether a RustDesk client
will render a `VideoFrame` smaller than the display, and where it would place
it. Read the client's decoder before writing any of it. If it does not work, the
combination of 1, 2 and 4 is the realistic ceiling.

## 5. Already ruled out

Do not re-litigate these; each cost real time to establish.

* **`CGDisplayCapture` to "speed up" or "refresh" capture.** It is unnecessary --
  the mapping is live -- and actively harmful: it seizes the display, registers
  the agent as an application, steals focus and dismisses menus. See
  `src/capture.rs`.
* **Reading the framebuffer per pixel.** 6364 ms per frame against 350 ms for a
  bulk copy. The read must be one `memcpy` per band.
* **libyuv's `ARGBToI420`.** Its `ARGB` means little-endian word order, i.e.
  B,G,R,A in memory; this framebuffer is genuinely A,R,G,B. It would silently
  swap red and blue.
* **A faster `cpu_used`.** Already at the fastest the codec accepts, with the
  realtime deadline and no lookahead.

## 6. How to measure

```bash
ssh ppctiger '~/rustdesk-agent --probe-display'     # per-stage timings
PPC_HOST=ppctiger probes/run.sh vram-vs-ram read-scaling
cargo run --example probe_client -- 192.168.99.116:21118 ppctest123
```

Two cautions learned the hard way:

* **Do not measure with a client connected.** The agent serves one peer at a
  time and the encode path is shared; `probe_client` will simply fail.
* **Beware measuring an idle desktop.** Frame-rate figures from a screen that is
  not changing measure nothing. Force a change, and know what change you forced
  -- `killall Dock` is throttled by launchd to one respawn per ten seconds,
  which has already produced one wrong conclusion in this project.
