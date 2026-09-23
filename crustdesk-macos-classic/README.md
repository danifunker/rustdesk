# crustdesk-macos-classic

A RustDesk **agent** -- the controlled side -- for classic Mac OS, System 7.5.5
and later on 68k Macs. The application is called **C-Desk-Vint**: it is a C
program, where the other vintage agents in this repository are Rust built with
mrustc.

**Status:** serves a stock RustDesk client (1.4.9 tested) from QEMU's Quadra
800 under System 7.5.5 with MacTCP: login, picture, pointer, clicks, drags,
menus, keys. Not yet run on real hardware. See *What is known and what is not*.

## Why C, and why a VP8 encoder of its own

A stock RustDesk client decodes VP8, VP9, AV1 and H.26x -- its `RGB` and `YUV`
frame types exist in the protocol but no native client decodes them -- so an
agent has to send one of those. libvpx on a 33 MHz 68040 is not a serious
proposition, and neither is Rust's std on Retro68's newlib with no threads and
no sockets. So:

- **`src/core/vp8enc.c`, VP8-lite.** A VP8 encoder that uses the least of the
  format a screen needs: an unchanged macroblock is an inter macroblock with no
  motion and no coefficients (a fraction of a bit); a changed one takes the
  closer of the last frame and 16x16 intra prediction; one quantiser, the
  default probabilities, no loop filter. With no loop filter the encoder's
  reconstruction is exactly what the peer decodes, which is what makes skipping
  unchanged macroblocks safe for ever -- and `host/test_vp8.c` checks it,
  decoding every frame with libvpx and comparing byte for byte. Its tables are
  generated from RFC 6386 by `tools/gen_vp8_tables.py`, not typed in.
- **`src/core/session.c`**, the RustDesk protocol as a state machine with no
  I/O: the platform feeds it bytes and drains its queues. Direct-IP login,
  PeerInfo (claiming "Mac OS" 1.4.5, like the other vintage agents), mouse,
  keys, keepalive, refresh.
- **`src/core/pb.c`, `sha256.c`, `yuv.c`**: protobuf, the login hash, and
  colour conversion from every depth a Mac offers -- 1/2/4/8-bit indexed
  through the device's colour table, 16 and 32 bit direct.

All of `src/core` builds on Linux too; `host/` has the tests, a host agent
that serves a synthetic 8-bit desktop through the same code, and
`host/cdvpoke.py`, a scriptable peer.

## The Mac side

`src/mac/`:

| | |
|---|---|
| `net.c`, `mactcp.h` | MacTCP through the .IPP driver, all asynchronous and polled. Our own declarations of the interface; also works under Open Transport's MacTCP compatibility. |
| `screen.c` | The main GDevice's framebuffer, read directly, against a shadow copy, a macroblock row at a time. |
| `input.c` | Mouse through the low-memory globals plus MiniVNC's MBTicks trick (so drags and menus work, not just clicks); keys by PPostEvent. A client in Map mode sends Mac virtual keycodes, which are the ADB codes this machine uses. |
| `engine.c`, `glue.s` | The agent, run from a **deferred task** on a 20 ms Time Manager heartbeat. |
| `main.c` | The window, the prefs file, the log file, and the chores that may not happen at interrupt time. |

**Why a deferred task.** A background application on System 7 gets no time
while the frontmost one tracks a menu, runs a modal dialog, or just doesn't
call WaitNextEvent. An agent in its event loop stops too -- and a remote press
on the menu bar opens a menu that sticks, because the release is one of the
things it never gets to. Interrupts keep running, so the engine runs from
there, in slices of a couple of ticks: a remote press on the Apple menu opens
it, tracks the remote pointer, and releasing on an item chooses it. The rules
this imposes (no Memory Manager, no printf, no synchronous calls; the encoder
keeps its scratch off the stack) are at the top of `engine.h`.

MiniVNC ([marciot/mac-minivnc](https://github.com/marciot/mac-minivnc)) was
the map for the Mac side: the input techniques are its (and ChromiVNC's
before it), and so is the idea of doing the work at interrupt time.

## Building

```sh
cmake -S . -B build-m68k \
  -DCMAKE_TOOLCHAIN_FILE=$RETRO68/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build build-m68k          # build-m68k/C-Desk-Vint.bin, .dsk
scripts/make-disk.sh              # build-m68k/C-Desk-Vint.hda, for a BlueSCSI
make -C host test                 # the portable core against libvpx
```

Retro68 with its default Multiversal Interfaces is enough; Apple's Universal
Interfaces are not needed.

## Running it

On the Mac, run C-Desk-Vint (or put it in Startup Items). Its window shows the
address and the password; in a RustDesk client, type the address into the ID
field. `docs/READ-ME-MAC.txt` is the user's Read Me, and goes on the disk.

In QEMU, `scripts/q800.sh start` installs the build into Startup Items on a
System 7.5.5 disk and boots a Quadra 800 with the agent forwarded to
`127.0.0.1:31119`; `ICOUNT=5` paces the CPU like a real 33 MHz 68040.
`docs/TESTING.md` has the rest.

## What is known and what is not

Verified on QEMU's Quadra 800, System 7.5.5, MacTCP 2.0.6, 640x480x8:

| | |
|---|---|
| RustDesk 1.4.9 and `probe_client`: login, PeerInfo, picture in the right colours | works |
| Pointer, clicks, double-clicks, press-drag-release through a menu | works |
| Keys: Map-mode keycodes, held modifiers, Legacy characters, into Note Pad | works |
| Reconnecting, including while a menu holds the machine | works |
| Every frame equal to libvpx's decode of it (host tests, 4 sizes, q 0-127) | works |
| Switching depth mid-session in Monitors: 256 -> millions -> black & white -> 16, the peer's decoded picture checked against the Mac's screen at each | works |
| Colours as the Mac's user sees them (the driver's gamma table applied) | works |

Not established:

- **Real hardware.** Everything above is QEMU. Timings under `ICOUNT=5` are an
  estimate of a 33 MHz 68040, not a measurement of one.
- **Speed.** A keyframe of a patterned desktop is several seconds of 68040
  time: every macroblock of a dither carries coefficients. Small changes are
  quick. See RESUME.md for numbers and the plan.
- **Thousands of colours (16-bit)**: QEMU's Quadra offers 1, 4, 8 and 24
  bits, so the 16-bit path has only host tests.
- Open Transport, PowerPC, encryption, rendezvous by ID, clipboard: not yet.
