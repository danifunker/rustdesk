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

Done: encryption (signed_id/public_key, secretbox), rendezvous by ID through
hbbs/hbbr, LAN discovery (UDP 21119), the clipboard both ways, the pointer
shape, and reporting to a console (sysinfo, heartbeat) over https.

- **The local-address path** (hbbs sends FetchLocalAddr when peer and Mac
  share a public address, and the peer comes to port 21120) is untested:
  QEMU's NAT hides the Mac. The first real-LAN run will say.
- **TLS on a 68040** costs ~11 s of paced-QEMU time per new connection (two
  P-384 and one P-256 signature checks, and an X25519). The connection is
  kept, so it is rare, but it freezes that Mac's own UI while it happens.
  Levers: anchor Let's Encrypt's current intermediates too (one P-384 check
  fewer); compare BearSSL's m15 and m31 EC code on the 68040; session
  resumption if a console's TLS server offers session IDs.
- The console's `disconnect` and `strategy` answers are ignored, as the
  PowerPC agent ignores them.

## 7. Smaller things

- The Control Strip is standard on PowerBooks with 7.5 and on every Mac from
  7.6; a desktop 7.5.5 Mac needs it installed. Classic under Mac OS X has
  none (the installer says so).
- The refresh visits one macroblock row every 400 ms; after a keyframe it
  could go faster until everything has had its pass.
- Scroll wheel: there is none on System 7; arrow keys or page keys might
  stand in.
- A Settings change that restarts sharing takes a few seconds before the
  console is told again (the resolver is asked afresh).
