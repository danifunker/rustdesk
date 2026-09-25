Continue the classic Mac OS RustDesk agent, C-Desk-Vint. Read
`/home/dani/repos/rustdesk/crustdesk-macos-classic/RESUME.md` first -- it is
the source of truth for what is built, what is measured, and the traps that
cost time once (sections 2a-2e cover the ID server, the console, the
installer and menus, chat/screenshots, zstd, file transfer and the two
session slots). Do not re-derive anything it records.

**Where it stands.** One fat application, the 68k half for System 7.5.5+ and
the native PowerPC half for Mac OS 8/9 (and Classic under Mac OS X). By ID
through an ID server and relay, encrypted, or by IP; listed in a console over
https (BearSSL); clipboard both ways (zstd), chat, screenshots, the wheel,
the client's quality/FPS, restart; file transfer (MacBinary out, MacBinary
and BinHex in) beside a desktop session; installer, Settings, Sharing menu,
Control Strip module, a keep-awake option; quits at Shut Down; never blocks
on a missing network.

**Proven, and how.** In QEMU: the Quadra 800 (7.5.5, MacTCP) and mac99 (9.2.2,
Open Transport), driven by `host/cdvpoke.py` (desktop and `--files` file
manager) and, until this machine's client window stopped drawing, by RustDesk
1.4.9 by ID through the user's server. On real hardware: the PowerPC half
under Classic on the user's PowerBook G4, by IP and by ID; the user has
reported sessions there as flaky, cause unknown. **Not yet proven**: file
transfer from a real client's file manager (only cdvpoke has driven it), the
local-address path (QEMU's NAT hides it), anything on the real Quadra.

Priorities, in order:

1. **What the user reports.** Their PowerBook sessions drop now and then;
   the "C-Desk-Vint Log" file in the Preferences folder says why each
   connection ended -- ask for it, read it before guessing. The About box
   (modal) was ruled out: sessions carry on under it.

2. **Publishing the CI build.** The pipeline (RESUME.md 2f) is green. Run
   36088877830 (commit 4c7fc5780) was downloaded, its checksums and the
   private-string check (with the key, locally) passed, and its own app ran
   in QEMU on 7.5.5 and 9.2.2 (sessions, clipboard, screenshot, file round
   trip). The user publishes it; ask before touching a release (public).
   Their PowerBook drops: that build fixes a 42-byte line written into a
   40-byte stack buffer at deferred-task time on an ID-server timeout --
   maybe the cause, unproven; ask for the log if drops continue.

3. **Speed on the 68040** (docs/BACKLOG.md): ~11 s per new TLS connection to
   the console, 2.1 s per keyframe. Measure under `ICOUNT=5` before and
   after; keep `make -C host test` passing.

Boundaries: the user's toolchain, the DOOM port's workspace
(~/doom-mac-testenv) and the OS 9 image are shared -- read them, never change
them (the OS 9 image is only ever an overlay's backing file). MPW's headers
are reference only. Commit to `vintage-agents` with a `crustdesk-macos-classic:`
subject, in the house voice, with the Co-Authored-By line; never push unless
asked. **Builds for sharing carry no personal details**: the repo's
build-m68k/build-ppc are the generic ones (public ID server, no console);
the user's own server, key and console go only into the separate personal
build (RESUME.md section 5), and every served generic file is checked with
`tools/find-strings.py --must-not ...` for their hostnames and key -- NOT
`grep -c -a`, which is blind in the 68k half (RESUME.md section 4).

How to work: `make -C host test` after any core change. `scripts/q800.sh start`
(NET=restricted / NET=none to take the network away, QEMU_EXTRA for QEMU's
exception log), then `host/cdvpoke.py 127.0.0.1:31119 classic ...` to drive it
and `scripts/q800.sh shot NAME` to look; `scripts/mac99.sh` likewise for OS 9
(`ITEMS=... start` then `install` for the installer; `resume` after a restart
inside Mac OS 9). Packages: `scripts/make-disk.sh [-b BUILD-PARENT] -o X.hda -z
X.sit.hqx`. Say plainly what was measured in the emulator, what on real
hardware, and what not at all.
