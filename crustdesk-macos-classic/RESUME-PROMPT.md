Continue the classic Mac OS RustDesk agent, C-Desk-Vint. Read
`/home/dani/repos/rustdesk/crustdesk-macos-classic/RESUME.md` first -- it is
the source of truth for what is built, what is measured, and the traps that
cost time once. Do not re-derive anything it records.

**It works in the emulators.** One fat application: the 68k half on QEMU's
Quadra 800 under System 7.5.5, the native PowerPC half on QEMU's mac99 under
Mac OS 9.2.2. Picture, pointer, menus, keys, reconnects, every depth. Nothing
has run on real hardware yet; the user has a Quadra 800 and a BlueSCSI, and
`scripts/make-disk.sh` makes the disk image for it.

Priorities, in order:

1. **Whatever the real Quadra says.** If the user reports back, that outranks
   everything below. "C-Desk-Vint Log" in the Preferences folder has the
   engine's story, keyframe timings included.

2. **Keyframe speed** (docs/BACKLOG.md item 2). 2.1 s of 68040 time for the
   default desktop, on every connect and refresh, half of it tokens. 68k
   assembly for the token loop and transforms next. Measure under `ICOUNT=5` before and
   after, and keep `make -C host test` passing: it is the proof that the
   bitstream is still exactly what libvpx decodes.

3. **A real hardware cursor** (G3/G4 with an ATI card): the separate-pointer
   path has only run forced. Then encryption and rendezvous by ID, as the
   other vintage agents do.

Boundaries: the user's toolchain and the DOOM port's emulator workspace are
shared -- read them, do not change them. MPW's headers are reference only.
Commit to `vintage-agents` with a `crustdesk-macos-classic:` subject, in the
house voice; do not push unless asked.

How to work: `make -C host test` after any core change. `scripts/q800.sh
start`, then `host/cdvpoke.py 127.0.0.1:31119 classic ...` to drive it and
`scripts/q800.sh shot NAME` to look. Say plainly what was measured in the
emulator and what was not measured at all.
