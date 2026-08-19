**Title:** X server stops responding during sustained framebuffer capture (ReadDisplay); Newport pixel DMA never completes

---

While porting a screen-sharing agent to IRIX I hit a reproducible hang: after a
few seconds of capturing the framebuffer through SGI's ReadDisplay extension,
`Xsgi` stops answering entirely and never recovers. I have narrowed it down as
far as I can from the guest side and it looks like an emulation issue rather
than a guest-software one, but I would not be surprised to be wrong.

### Environment

- iris `4dc2e94`, built `--features lightning,rex-jit,r5k,chd`
- Host: Ubuntu 24.04, x86-64, 6 cores
- Guest: IRIX 6.5.22m, IP22 / R5000, 1280x1024 at depth 8
- Run as `iris --config iris.toml --ci --ci-display`

### What happens

`Xsgi` stays in `ps` but stops doing anything at all:

- **CPU time frozen** — unchanged for as long as you watch.
- `par -s -SS -p <pid>` records **zero syscalls over 60 seconds**. Not spinning,
  not polling: asleep in the kernel and never waking.
- `netstat -an` shows a new client's bytes **unread in the server's receive
  queue** (`recv-Q 12` for a 12-byte X11 setup request). The kernel completed the
  TCP handshake from the listen backlog; the server never read it.
- Every client hangs, including IRIX's own `xdpyinfo`. After the server's
  60-second authorisation timeout the connection is dropped, so a later
  `XOpenDisplay` returns NULL rather than hanging — which is why this initially
  looked like a client bug to me.

Sometimes — not always — the guest kernel prints on the serial console:

```
WARNING: ng1 pixel dma read timeout
WARNING: ng1: pixel dma timeout!
```

What strikes me about that is that the Newport driver *detected* the timeout and
returned, and yet the process still never became runnable again. That reads more
like a wakeup or interrupt that never arrives than a register returning a wrong
value, but I do not know the driver well enough to say.

Killing `Xsgi` and starting a fresh one always recovers; capture then works
immediately, including from a brand-new client process.

### Reproducing it

Honestly: **it is intermittent, and I could not isolate it into a small test.**
I wrote a ~100-line standalone client to try, and the negative results may be
more useful than the positive one. Measured on this image:

| what | result |
|---|---|
| 50 back-to-back full-screen `XShmReadDisplayRects` (1280x1024) | no stall |
| 40 of the same with `XRD_READ_POINTER` (cursor composited) | no stall |
| 40 `SGICapQueryCopyAndReset` damage polls (SGI-SCREEN-CAPTURE) | no stall |
| 12 full-screen reads with a 5 s CPU burn between each | stalled once; a second identical run completed all 12 |

So it is not read volume alone, not cursor compositing, and not the damage
extension. The only pattern that has produced it outside the real workload
involves a multi-second CPU-bound gap between reads — which is what a video
encoder does between frames — and even that is not reliable.

What *does* reproduce it, five sessions out of five, is the real workload:
capture a frame, spend several seconds encoding VP8, repeat, with a second X
connection open for input injection via XTEST. It wedges after exactly one
delivered frame every time.

I can share the standalone probe and the full agent if either would help.

### Probably not the lead

Every run logs these, and I chased them for a while before noticing the counts
are byte-identical between runs that stayed healthy for hours and runs that died
within one frame — they look like deterministic probes at X startup:

```
8 x REX3 Read32: unhandled reg 007c
8 x MC: GIO Timeout at 1f46a07c
3 x MC: GIO Timeout at 1f400000
3 x MC: GIO Timeout at 1f600000
```

### Things I ruled out

- **The screen saver.** Armed at 45 s; the screen blanked and the server stayed
  healthy well past it.
- **Which server binary.** Happens with the one xdm starts
  (`-bs -nobitscale -c -pseudomap 4sight -solidroot sgilightblue`) and with a
  bare `Xsgi :0 -bs -c` with no window manager and nothing drawing.
- **Anything on the client side.** I verified this with a client that speaks the
  X11 connection setup by hand over a plain socket with no Xlib at all: against a
  wedged server it gets no answer and no EOF; against a fresh one it gets
  `success=1` immediately.

### Where I might look, for what it is worth

I have not read enough of `rex3.rs` to have a real opinion, so please treat these
as a starting point rather than a suggestion:

- `process_pixel_read` (`src/rex3.rs:2804`), the host-read path ReadDisplay
  drives.
- `REX3_STATUS` (`src/rex3.rs:148`) and its bits (`src/rex3.rs:426`) —
  `STATUS_GFXBUSY`, `STATUS_BACKBUSY`, and the GFIFO/BFIFO level fields. If the
  driver polls one of those after issuing a pixel read and the emulated state
  leaves it looking permanently busy, that would fit both the kernel's timeout
  message and the process never waking.
- Whatever interrupt the Newport driver expects on read completion.

### Even partial progress would help

The agent already recovers from a *dead* X connection by reconnecting. So even
turning this silent wedge into a clean connection drop would be a real
improvement from my side, if the underlying completion problem turns out to be
hard.

Happy to test patches — I have the setup standing and can turn a build around
quickly.
