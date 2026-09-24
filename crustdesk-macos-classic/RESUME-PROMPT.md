Continue the classic Mac OS RustDesk agent, C-Desk-Vint. Read
`/home/dani/repos/rustdesk/crustdesk-macos-classic/RESUME.md` first -- it is
the source of truth for what is built, what is measured, and the traps that
cost time once. Do not re-derive anything it records.

**It works in the emulators, and the PowerPC half on a real G4.** One fat
application: the 68k half on QEMU's Quadra 800 under System 7.5.5, the native
PowerPC half on QEMU's mac99 under Mac OS 9.2.2 and on the user's PowerBook G4
(Classic under Tiger). By ID through the user's ID server and relay,
encrypted; listed in their console over https; installer, Settings dialog,
Sharing menu, Control Strip module. The user has a Quadra 800 and a BlueSCSI;
`scripts/make-disk.sh` makes the disk image, `-z` the .sit.hqx they pull from
a LAN web server.

Priorities, in order:

1. **Whatever the real machines say.** If the user reports back, that
   outranks everything below. "C-Desk-Vint Log" in the Preferences folder has
   the engine's story, keyframe and TLS timings included. The local-address
   path (same LAN as the client) has never run: the emulators' NAT hides it.

2. **TLS and keyframe speed on the 68040** (docs/BACKLOG.md items 6 and 2):
   ~11 s per new console connection, 2.1 s per keyframe. Measure under
   `ICOUNT=5` before and after, and keep `make -C host test` passing: it is
   the proof that the bitstream is still exactly what libvpx decodes.

3. **A real hardware cursor** (G3/G4 with an ATI card): the separate-pointer
   path has only run forced.

Boundaries: the user's toolchain and the DOOM port's emulator workspace are
shared -- read them, do not change them. MPW's headers are reference only.
Commit to `vintage-agents` with a `crustdesk-macos-classic:` subject, in the
house voice; do not push unless asked.

How to work: `make -C host test` after any core change. `scripts/q800.sh
start`, then `host/cdvpoke.py 127.0.0.1:31119 classic ...` to drive it and
`scripts/q800.sh shot NAME` to look; `scripts/mac99.sh` likewise for OS 9
(`ITEMS=... start`, then `install`, to test the installer). The user's builds
set `-DCDV_SERVER`, `-DCDV_KEY` (from ~/.config/rustdesk/RustDesk2.toml) and
`-DCDV_API=https://remote.home.dani.tech` in a separate build directory. Say plainly what was measured in the
emulator and what was not measured at all.
