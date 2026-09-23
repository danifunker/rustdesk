# Backlog

Roughly in order of what a user would notice.

## 1. Real hardware

The Quadra 800 over BlueSCSI: `scripts/make-disk.sh` makes the disk. Worth
learning there, in this order: does it start and get an address; does a
client connect; how long a keyframe takes (the log file says); whether the
input tricks hold on a real ADB machine (they are MiniVNC's and ChromiVNC's,
proven on hardware, but not by us); how reading VRAM through the Quadra's
DAFB video compares with QEMU's.

## 2. Keyframe speed

A keyframe of the default 7.5.5 desktop is about 4 s of emulated 68040 time,
and every new session and refresh starts with one. Per-macroblock work, not
entropy coding, is the cost (a coarser quantiser saves bytes, not time).
Levers, cheapest first:

- **Flat macroblocks.** Most of a desktop is single-colour fills. A block whose
  residual is constant has only a DC coefficient: skip the transforms, and
  skip mode search once a prediction is exact.
- **68k assembly** for the DCT/IDCT and SAD, register-allocated by hand as
  MiniVNC's TRLE encoder is.
- **Patterns by motion vector.** A dithered desktop is the same 16x16 block
  twelve hundred times, and VP8 can only reuse one through inter prediction.
  After a cheap keyframe, a few inter frames can double the correct area each
  time with a NEWMV pointing at a copy already made. Needs motion vector
  coding and the near/nearest census with nonzero vectors -- the libvpx
  comparison test will say whether it is right.
- **VBL-time checksums** (MiniVNC) instead of comparing VRAM with a shadow,
  which halves memory traffic and saves the shadow's RAM.

## 3. Colour depths

Only 256 colours has been run on a Mac. Thousands and millions go through
`yuv.c`'s direct paths (host-tested); 1-bit through the indexed path with a
2-entry table. Try each in the Monitors control panel, including switching
depth mid-session (the engine suspends, rebuilds and sends SwitchDisplay).

## 4. PowerPC and a fat binary

Retro68 builds PowerPC (PEF, InterfaceLib); `mactcp.h` already packs to 68k
alignment there. The engine's glue is 68k assembly and the heartbeat is the
Time Manager: on PowerPC, a Time Manager task and deferred task are both
available through Mixed Mode, or the engine can run from the event loop plus
a Thread Manager thread. Then combine the 68k CODE and the PEF into one file.
Mac OS 8.6+ may draw the pointer in hardware: then it is not in VRAM, and
the agent must send CursorData.

## 5. Open Transport, natively

The MacTCP interface works under Open Transport's compatibility layer; native
OT is faster on PowerPC. Needs the Universal Interfaces (MPW-GM) or our own
declarations of the OT calls used.

## 6. Protocol

- Encryption: the signed_id/public_key exchange and secretbox, as the other
  agents do. X25519 and Ed25519 in portable C; benchmark on the 68040 first.
- Rendezvous (by ID through hbbs/hbbr) and LAN discovery (UDP 21119).
- Clipboard: Scrap Manager TEXT <-> UTF-8 via Mac Roman.
- Screenshots (a PNG of the shadow), a cursor shape when the pointer is not
  embedded.

## 7. Smaller things

- An icon, a BNDL, and "C-Desk-Vint" as a proper document creator.
- A settings dialog, so the password can be changed without SimpleText.
- The refresh visits one macroblock row every 400 ms; after a keyframe it
  could go faster until everything has had its pass.
- Scroll wheel: there is none on System 7; arrow keys or page keys might
  stand in.
