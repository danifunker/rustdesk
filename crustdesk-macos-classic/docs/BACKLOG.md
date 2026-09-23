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

Done for 1, 4, 8 and 32 bits: each switched to live in the Monitors control
panel with a peer connected, and the peer's decoded stream compared with the
Mac's screen. Thousands of colours (16-bit) is untried on a Mac -- QEMU's
Quadra does not offer it -- and the gamma handling there expands 5-bit
components to 8 before the lookup, which is a guess at what the hardware does.

## 4. PowerPC on real hardware

Done in QEMU: the fat application runs natively on Mac OS 9.2.2 (mac99, G4).
Untested: a real G3/G4, Mac OS 8.x, and a real hardware cursor -- the path
that sends the pointer separately has only run forced (`cursor=separate`).
Worth a look on a G4: whether AltiVec is worth it for the transforms.

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
