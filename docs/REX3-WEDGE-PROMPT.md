# Prompt: the X server wedges under sustained ReadDisplay traffic

Hand this to a session working on `~/repos/iris`. The evidence below was
gathered from the guest side while porting a RustDesk agent to IRIX; nothing in
iris was modified to collect it.

---

Investigate why the IRIX X server stops responding when a client reads the
framebuffer repeatedly through SGI's ReadDisplay extension. It is the one thing
standing between a working remote-desktop agent and a usable one, and everything
above the X server has been ruled out.

## The symptom, precisely

`Xsgi` stops answering and never recovers. Specifically, and all measured:

- It stays in `ps` with **its CPU time frozen** — `0:11` for as long as you care
  to watch.
- `par -s -SS -p <pid>` records **zero syscalls over 60 seconds**. It is not
  spinning and not polling; it is asleep in the kernel and never wakes.
- `netstat -an` shows the client's bytes sitting **unread in the server's socket
  receive queue** (`recv-Q 12` for a 12-byte X11 setup request). The kernel
  completed the TCP handshake from the listen backlog; the server never called
  `accept` or `read`.
- Every client then hangs, IRIX's own `xdpyinfo` included. After the server's
  60-second authorisation timeout the connection is dropped, so `XOpenDisplay`
  eventually returns NULL and a client that retries logs a connect failure
  rather than a hang — which is why this first looked like a client bug.

Sometimes, and only sometimes, the guest kernel prints on the serial console:

```
WARNING: ng1 pixel dma read timeout
WARNING: ng1: pixel dma timeout!
```

`ng1` is the Newport driver. Note what that implies: **the driver detected the
timeout and returned, yet the process never became runnable again.** A missing
wakeup or an interrupt that is never delivered fits that better than a register
that simply reads wrong.

## Reproducing it

**Be warned: it is intermittent, and no small probe isolates it.** I tried, and
the negative results are worth as much as the positive one.
`probes/rex3-wedge-repro.c` is a standalone ~100-line client with four modes;
measured on this image:

| what | result |
|---|---|
| 50 back-to-back full-screen `XShmReadDisplayRects` (1280x1024) | no stall |
| 40 of the same with `XRD_READ_POINTER` (cursor composited) | no stall |
| 40 `SGICapQueryCopyAndReset` damage polls | no stall |
| 12 full-screen reads with a **5 s CPU burn between each** | **stalled once**, then a second identical run completed all 12 |

So it is not the read volume on its own, not cursor compositing, and not the
damage extension. The one pattern that has produced it outside the real workload
involves a multi-second CPU-bound gap between reads — which is what a video
encoder does — but even that is not reliable.

**The workload that reproduces it nearly every time** is a real screen-sharing
session: capture, several seconds of VP8 encoding, repeat, with a second X
connection open for input injection. Five sessions out of five wedged after
exactly one delivered frame.

```sh
# host: build (no emulator needed, this is a cross-compile)
cd ~/repos/irix-rustdeskagent/ports/rust/agent-portable && . ../env.sh
cargo +nightly build --release          # ~37 s from clean

# guest: /root/agent-restart.sh starts X and the agent; then
LD_LIBRARYN32_PATH=/usr/sgug/lib32 /tmp/testpeer 127.0.0.1:21118 hunter2 120
```

The peer logs in, receives exactly **one** VP8 keyframe at about 23 s, and then
the server is gone for the rest of the session. Five times out of five.

Recovery is reliable: `/root/restart-x.sh` kills `Xsgi` by pid and starts a bare
`Xsgi :0 -bs -c`. Used four times, capture works immediately afterwards every
time — including rebuilding the client's `Capturer` from scratch.

## Already ruled out — please don't redo these

- **The screen saver.** Armed with `xset s 45 45 s blank`; the screen blanked and
  the server stayed healthy well past the timeout.
- **One oversized request.** I believed for a while that a single whole-screen
  `XShmReadDisplayRects` wedged it every time, and said so; that was wrong.
  Splitting reads into 128-row strips did fix the agent's `--probe-display`, but
  a loop of 50 whole-screen reads runs clean, so size is not the variable. Take
  any claim here that rests on "it always happens when X" with suspicion —
  including mine.
- **A redundant double read.** The client was re-reading all 64 bands right after
  a full read — a real bug, since fixed. It changed nothing here.
- **Which server binary.** Happens with xdm's `Xsgi -bs -nobitscale -c -pseudomap
  4sight -solidroot sgilightblue` and with a bare `Xsgi :0 -bs -c` that has no
  window manager and nothing drawing.
- **Anything client-side.** Toolchain, Xlib, the SGI shm transport, `-Bsymbolic`,
  and access control were all eliminated with `probes/rawx.c`, which speaks the
  X11 setup by hand over a plain socket with no Xlib at all. Against a wedged
  server it gets no answer and no EOF; against a fresh one it gets `success=1`.

## A correction worth having before you start

`iris-*.log` shows these on every run:

```
8 x REX3 Read32: unhandled reg 007c
8 x MC: GIO Timeout at 1f46a07c
3 x MC: GIO Timeout at 1f400000
3 x MC: GIO Timeout at 1f600000
```

**These are not the wedge.** The counts are byte-identical across runs that
stayed healthy for hours and runs that wedged within a frame — they are
deterministic probes at X startup. Worth explaining eventually; not the lead.

## Where I would look

I have not read enough of the REX3 emulation to have a strong opinion, so treat
these as pointers rather than a diagnosis:

- `src/rex3.rs:2804` `process_pixel_read` — the host-read path ReadDisplay
  ultimately drives.
- `src/rex3.rs:148` `REX3_STATUS`, and the bits at `src/rex3.rs:426` —
  `STATUS_GFXBUSY` (1<<3), `STATUS_BACKBUSY` (1<<4), and the GFIFO/BFIFO level
  fields. If the driver polls a busy or FIFO-level bit after issuing a pixel
  read and the emulation leaves it in a state the driver reads as "never
  finished", that matches both the kernel's timeout message and the process
  never waking.
- `src/rex3.rs:5295` `dma_read64`.
- Whichever interrupt the Newport driver expects on read completion — the
  process staying asleep after the driver logged a timeout points at a wakeup
  that never arrives.

The most useful first measurement is probably: instrument what the guest reads
from `REX3_STATUS` in the seconds before the wedge, and compare a healthy
strip-sized read against the read that kills it.

## What done looks like

A screen-sharing session that runs for minutes and delivers tens of frames
through `XShmReadDisplayRects` without the server going quiet — `testpeer`
against the agent is the measure, since the small probes do not provoke it. The agent already handles a *dead* X server by reconnecting,
so partial progress is useful: even turning the silent wedge into a clean
connection drop would be an improvement, because the agent recovers from that.

## Boundaries

- **Another Claude session may be using this machine.** It has its own emulator
  and image under `~/repos/rust-irixlibstd/scratch/emu`. `pkill -x iris` kills
  every iris on the box — I did that and took down a 4h52m-old instance. Kill by
  the pid whose `/proc/<pid>/cmdline` names *your* config, and note that
  `ps | grep "iris --config"` also matches the bash wrapper, so killing the match
  can leave the emulator running and produce two instances fighting over one CHD
  and one set of port forwards.
- **`~/repos/iris/target/release/iris` currently has no `chd` feature** (rebuilt
  2026-08-19 00:08 with only `tlbvmap`), so it cannot open either image. Rebuild
  with `--features lightning,rex-jit,r5k,chd` — 5m33s. A working copy built from
  that same source is at
  `~/repos/irix-rustdeskagent/ports/iris-run/iris-target/release/iris`.
- The agent's disk is `~/Indy-IRIX65_dev.chd`. Changes must be folded in with
  `chdman copy` and verified; see `RESUME.md` §Disk handling. Keep
  `Indy-IRIX65_dev.chd.bak-before-first-write`.
- `iris-ci start` is required under `--ci` — the CPU thread is created paused,
  and a machine that has not been started is indistinguishable from a slow boot.

## Please be straight about what you find

If it turns out to be a guest-side driver quirk rather than an emulation gap, or
if the reproduction does not hold on your setup, say so plainly. An accurate
description of a failure is worth more here than a hopeful one — the last three
sessions each lost time to a confident wrong diagnosis, including mine.
