Continue the IRIX 5.3 RustDesk agent, C-Desk-Vint for IRIX. Read
`/home/dani/repos/rustdesk/crustdesk-irix/RESUME.md` first -- it is the
source of truth for the build (GCC objects reshaped for SGI's ld, linked on
the machine), the design (XTrap damage, overlays, motion vectors, the frame
budget), the measurements and the traps. Do not re-derive anything it
records; the portable core it shares is `../crustdesk-macos-classic/src/core`
(run `make -C ../crustdesk-macos-classic/host test` after touching it, and
keep the Mac builds compiling).

**Where it stands.** On the user's R3000 IRIS Indigo (IRIX 5.3, LG1, 8-bit):
desktop, menus (overlay planes), keys and pointer, clipboard both ways, file
manager, scrolls and window moves as motion vectors, a frame budget that
keeps big changes at 2.5-3 fps, reconnecting across a logout, remote login at
clogin, and a tar package with install.sh. Proven with
`host/cdvpoke.py` by IP only.

Priorities, in order:

1. **What the user decides** (they were asked): a default route and DNS on the
   Indigo (it reaches only 192.168.99.0/24), so the ID server can be tested;
   whether to run `install.sh` on it (boot start through chkconfig); whether a
   real RustDesk client may be driven on their desktop.
2. **A real client**, then the ID server and relay.
3. **Speed** (RESUME.md section 7): measure with `bench/encbench.c` and
   `prof` on the Indigo before and after.

Boundaries: the Indigo is the user's machine -- its network and boot
configuration are theirs to change; clean up what testing leaves in
`/usr/tmp`. The IRIX 5.3 headers are SGI's: they stay in `build/`. Shareable
builds carry no personal details (the default ID server is RustDesk's public
one); the user's own server goes only in a prefs file on the machine.
Commit to `vintage-agents` with a `crustdesk-irix:` subject in the house
voice, with the Co-Authored-By line; never push unless asked.

How to work: `scripts/deploy.sh` (make, link on the Indigo, restart the test
agent), then cdvpoke to drive it and `vp8dump` to look. Say plainly what was
measured on the Indigo and what not at all.
