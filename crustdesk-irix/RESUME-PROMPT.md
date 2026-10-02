Continue the IRIX 5.3 RustDesk agent, C-Desk-Vint for IRIX. Read
`/home/dani/repos/rustdesk/crustdesk-irix/RESUME.md` first -- it is the
source of truth for the build (GCC objects reshaped for SGI's ld, linked on
the machine), the design, the measurements and the traps; **section 7 says
what the second session did and what is left**. Do not re-derive anything it
records. The core is shared with the classic Mac agent
(`../crustdesk-macos-classic/src/core`): run `make -C
../crustdesk-macos-classic/host test` after touching it and keep the Retro68
builds compiling.

**Where it stands.** Released: build 20261002-d061a0d3a (tar, tardist,
SHA256SUMS, MANIFEST) in `r-deskway-1.5.0`, a copy in
`~/vintage-state/crustdesk-irix/`. It is installed by `inst` on the user's
R3000 IRIS Indigo (IRIX 5.3, LG1 8-bit, `192.168.99.38`, telnet root), starts
at boot, registers with the user's server by name, reports to their console
over https, and has a settings panel in the Toolchest. The Indigo now has a
route, DNS and a right clock.

What is open, for the user to choose from (ask; none is decided):

1. **Cursor shapes** (RESUME section 7, item 6): following pointer grabs
   through XTrap would show IRIX's launch feedback, menus and 4Dwm's
   move/resize cursors; window cursors cannot be read back from X. Needs
   the cursor font's glyphs carried in the agent.
2. **A 24-bit SGI** to test on: only Xvfb at depth 24 stood in. The user said
   "not now" to O2 access on 2026-10-02; ask again before assuming.
3. **Speed**: coded text is ~2.7 ms a macroblock on the R3000; a pass that
   sharpens what was sent coarsely.
4. **The Mac**: "none" as its console user (for the file manager) and the
   new Let's Encrypt anchors are committed but not in a Mac release.

Boundaries: the Indigo is the user's. Its prefs file holds their own server,
key and console -- personal settings that stay in that file only; shareable
builds carry the public ID server and no console. Clean up what testing
leaves in /usr/tmp (the development copy in `/usr/tmp/cdv-build` with its
test prefs is kept on purpose). **A reboot empties /tmp** (IRIX does it at
boot): look and ask before rebooting. SGI's headers stay in `build/`. Commit to `vintage-agents` with a
`crustdesk-irix:` subject in the house voice, with the Co-Authored-By line;
never push unless asked; ask before uploading to a release (notes are CRLF).

How to work: `scripts/fetch-toolchain.sh` and `IRIX53_IMAGE=~/Indy-IRIX53_dev.chd
scripts/make-sysroot.sh` first (both are cleaned away after a session), then
`scripts/deploy.sh`; `make gui` and `indigo.py linkobj build/gui
/usr/tmp/cdv-gui cdeskvint-panel` for the panel; `make dist` for the
packages; `indigo.py inst DIST 'cmd'...` to install them. Drive the screen
with `../crustdesk-macos-classic/host/cdvpoke.py` (exact coordinates; `save`
then `host/build/vp8dump`), the real client with `rustdesk --connect ADDR
--password PW` on this machine, captured with `xwd -id`. Say plainly what was
tested on the Indigo, with which client, and what not at all.
