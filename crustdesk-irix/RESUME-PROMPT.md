Continue the IRIX 5.3 RustDesk agent, C-Desk-Vint for IRIX. Read
`/home/dani/repos/rustdesk/crustdesk-irix/RESUME.md` first -- it is the
source of truth for the build (GCC objects reshaped for SGI's ld, linked on
the machine), the design, the measurements and the traps, and **section 7 is
this session's work list**, with what was found for each item. Do not
re-derive anything it records. The core is shared with the classic Mac agent
(`../crustdesk-macos-classic/src/core`): run `make -C
../crustdesk-macos-classic/host test` after touching it and keep the Retro68
builds compiling.

**Where it stands.** On the user's R3000 IRIS Indigo (IRIX 5.3, LG1, 8-bit,
`192.168.99.38`, telnet root): the desktop at 2-3.5 fps under load, menus in
the overlay planes, keys and pointer, clipboard, file manager, motion vectors,
logout and remote login; proven with RustDesk 1.4.9 by IP and by ID through
the user's server (via a temporary forwarder: the Indigo had no route out).
Build 20261002-f2609ae03 runs from /usr/tmp on it; its package is in
`~/vintage-state/crustdesk-irix/`.

The user's decisions, in order (details and pointers in RESUME.md section 7):

1. **Boot start via chkconfig** (approved): run `pkg/install.sh` on the Indigo,
   verify `chkconfig` shows `cdeskvint on`, reboot, verify it serves at the
   login screen.
2. **inst packaging**: make it installable with IRIX 5.3's `inst` -- gendist
   from `inst_dev.sw` on `/CDROM2/dist` (or irixscsitb's guest recipe), an
   exitop/removeop for chkconfig and start/stop. Keep the tar for machines
   without it.
3. **The Mac's file-manager username**: send "none" (user's choice); test.
4. **https and DNS** once the user has finished the Indigo's networking:
   servers by name; the console heartbeat over BearSSL in a forked child,
   `api=` in prefs, none by default.
5. **24-bit and larger screens**, chosen at run time (auto-detect, a prefs
   override, ReadDisplay for mixed depths, buffers from the real size). Ask
   for access to a 24-bit SGI (the O2 runs 6.5, which runs this binary).
6. **Pointer**: it stays an arrow (documented); investigate what IRIX does
   while it launches a program (the flashing pointer) and report whether
   cursor shapes are worth sending.
7. **Release** into the `r-deskway-1.5.0` version matrix (approved): the
   build that was installed and tested on the Indigo, inst image and tar,
   SHA256SUMS, notes saying exactly what was tested. Ask before uploading.

Boundaries: the Indigo is the user's -- its network is theirs; installing
inst_dev and C-Desk-Vint on it is in scope; clean up what testing leaves in
/usr/tmp and keep `/tmp/t.c`, `/tmp/t1`, `/tmp/t2` (the user's). SGI's headers
stay in `build/`. Shareable builds carry no personal details (public ID
server, no console); the user's server and console go only in a prefs file
on the machine. Commit to `vintage-agents` with a `crustdesk-irix:` subject in
the house voice, with the Co-Authored-By line; never push unless asked.

How to work: `scripts/fetch-toolchain.sh` and `IRIX53_IMAGE=~/Indy-IRIX53_dev.chd
scripts/make-sysroot.sh` first (both were cleaned away), then
`scripts/deploy.sh`, `../crustdesk-macos-classic/host/cdvpoke.py` to drive and
`host/build/vp8dump` to look; the real client is `rustdesk --connect ADDR
--password PW` on this machine, captured with `xwd -id`. Say plainly what was
tested on the Indigo, with which client, and what not at all.
