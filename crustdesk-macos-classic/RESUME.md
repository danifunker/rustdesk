# RESUME — C-Desk-Vint, RustDesk for classic Mac OS

State of the port, how it is built and tested, what is proven and what is
not, and the traps that cost time the first time. This is the source of
truth; `RESUME-PROMPT.md` is the short prompt that points here. `README.md`
explains the design; `docs/TESTING.md` the harness; `docs/BACKLOG.md` what
is next.

---

## 1. Where it stands

C-Desk-Vint serves a stock RustDesk client from QEMU's Quadra 800 (System
7.5.5, MacTCP 2.0.6) and, as the native half of the same fat application,
from QEMU's mac99 G4 (Mac OS 9.2.2, Open Transport): login with the
empty-password probe, the picture in the colours the Mac's user sees, the
pointer, clicks, double-clicks, press-drag-release through a menu, keys in
Map and Legacy modes, reconnects, and every depth switched live (1, 4, 8, 32
bits). The user has a real Quadra 800 and a BlueSCSI; `scripts/make-disk.sh`
makes the disk for it. **It has not run on real hardware yet.**

User decisions that shape it: target System 7.5.5 first, then MacTCP-era
systems; the MacTCP interface first (it also runs under Open Transport's
compatibility layer, so this covers 7.5.5-9.2.2); the directory is
`crustdesk-macos-classic` and every application it produces is named
**C-Desk-Vint**; C, not Rust.

## 2. The pieces, and why each is the way it is

- **VP8-lite** (`src/core/vp8enc.c`): the client only decodes VP8/VP9/AV1/
  H.26x, and libvpx is not an option on a 68040. No loop filter, so the
  encoder's reconstruction equals the decoder's; that is what makes skipping
  unchanged macroblocks correct, and `host/test_vp8.c` proves it against
  libvpx on every change. Tables come from RFC 6386 via
  `tools/gen_vp8_tables.py`. It encodes a row at a time
  (`vp8e_begin/rows/end`), and its per-macroblock scratch is in the struct.
- **Session** (`src/core/session.c`): no I/O. Control queue plus one video
  frame; a queue is claimed when peeked, so messages never interleave; video
  headers are built in front of the encoder's output, never copied.
- **Engine at deferred-task time** (`src/mac/engine.c`, `glue.s`): the fix
  for System 7's cooperative scheduling. Without it a remote menu press
  opened a menu that stuck (the agent never got time to process the release).
  Time Manager task every 20 ms -> DTInstall -> `engine_tick`, which does a
  slice of at most ~2 ticks. The main loop does allocation, the colour table,
  the KCHR pointer, the window and the log file.
- **Input**: MiniVNC's MBTicks trick keeps the ROM from overwriting MBState,
  so drags and menus work. Keys: the client sends Mac virtual keycodes in Map
  mode because PeerInfo says "Mac OS"; those are ADB codes; KeyTranslate
  gives the character.

## 3. Measured

Under `ICOUNT=5` (QEMU paced ~a 33 MHz 68040; guest time, see TESTING.md):

| | |
|---|---|
| keyframe, default 7.5.5 desktop (1047 of 1200 macroblocks coded) | scan 350 ms, encode 2.1 s (was 3.9 s), 157 KB |
| where it goes (CDV_PROFILE=ON), after the intra cache | mode 252, quant 201, recon 141, tokens 1089 ms |
| OS 9 on mac99 (G4, not paced): keyframe 800x600x32 | 116 ms |
| a small change (a few macroblocks) | 50-70 ms scan to send, 60-250 bytes |
| a still screen | scanned every 250 ms; nothing sent once refined |

On the host, a 640x480 dither keyframe is 9 ms; the cost is per macroblock
(transforms, mode search), not entropy coding: q 127 instead of 16 cuts the
bytes 5x and the time 25%.

## 4. Traps

- **Colour is after gamma.** The framebuffer holds what QuickDraw meant; the
  card's gamma table (cscGetGamma -- csParam *points at* the record) is what
  the monitor gets. Without it midtones reached the peer ~30 levels dark.
  QEMU's q800 display applies it in 8-bit; mac99 skips it in direct colour.
- **A fresh OS 9 overlay starts the Setup Assistant**, which eats keystrokes;
  `scripts/mac99.sh launch` quits it first.
- **`_PPostEvent` takes the event code in A0 and the message in D0.** Swapped,
  it posts null events and returns noErr. Apple's Events.h:
  `#pragma parameter __D0 PPostEvent(__A0, __D0, __A1)`.
- **Multiversal gaps**: no MacTCP.h (ours is `src/mac/mactcp.h`), no glue for
  PPostEvent, DTInstall, InsXTime (inline `.short` traps), no FSpOpenDF/
  FSpCreate (inline, selector in D0 on A A52), no `kOnSystemDisk`, no
  `true32b`. A function called `frame` collides with a Toolbox symbol.
- **Deferred-task rules**: no Memory Manager, no printf (newlib's needs too
  much of an interrupted stack), no synchronous driver calls (the stream is
  reset by an asynchronous TCPAbort that the engine finishes), no Script
  Manager (KCHR is cached by the main loop; KeyTranslate itself is fine),
  one CODE segment (`-Wl,--mac-single`) so nothing is loaded at interrupt
  time.
- **The status window is on the captured screen.** Erase-then-draw fed back
  into frames at 11 fps; it now draws over itself. Anything that animates
  there costs bandwidth.
- **A scan moves the shadow forward**: scan only when a frame can be sent,
  or changes are lost. The engine checks VID_FREE first.
- **`pkill -f` matching the command line kills the Bash tool's own shell**;
  kill QEMU by the pid in `$CDV_TESTENV/qemu.pid`, the host agent with
  `pgrep -x`.
- **Emulator time under -icount runs ahead of wall time**; only the Mac's own
  logged durations mean anything.

## 5. Environment

- Mac OS 9.2.2: `~/MacOS9-2-2 UTM.qcow2` (UTM install), used only as the
  backing file of an overlay in `~/cdv-testenv`.
- Retro68 at `~/repos/Retro68-build/toolchain` (Multiversal Interfaces; the
  DOOM port uses the same). MPW-GM is in `~/Downloads/MPW-GM.img.bin`, and
  `/tmp/mpw-gm/ref/` has Apple's MacTCP.h, Events.h, LowMem.h, Retrace.h,
  Script.h, Scrap.h extracted for reference (reference only: nothing is
  copied from them).
- QEMU 8.2 `qemu-system-m68k -M q800`; workspace `~/cdv-testenv` (TESTING.md).
- MiniVNC source at `/tmp/mac-minivnc` (GPLv3; ChromiVNC's keyboard code in
  it is GPLv2+). The map for the Mac side; nothing is copied from it.
- RustDesk 1.4.9 is installed on this machine; X11 at `:0`.
