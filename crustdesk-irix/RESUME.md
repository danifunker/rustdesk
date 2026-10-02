# RESUME — C-Desk-Vint for IRIX

State of the IRIX 5.3 port, how it is built and tested, what is proven and
what is not, and the traps that cost time the first time. This is the source
of truth; `RESUME-PROMPT.md` points here; `README.md` explains the build;
`pkg/README.txt` is the user's read-me.

---

## 1. Where it stands

C-Desk-Vint (the classic Mac agent's name: a C program, the Mac agent's
portable core) serves a RustDesk client from the user's **IRIS Indigo**:
R3000 at 33 MHz, 56 MB, LG1 (Entry) graphics at 1024x768x8, IRIX 5.3
(`IRIX INDIGOR3K 5.3 12200157 IP12`). The other vintage IRIX agent
(`../rustdesk-irix65-agent`, Rust, n32) needs IRIX 6.5 and MIPS III; nothing
there runs on 5.3, which is o32 only, and an R3000 is MIPS I.

Proven on the Indigo, driven by `../crustdesk-macos-classic/host/cdvpoke.py`
by IP, every frame decoded by libvpx (`host/build/vp8dump`):

| | |
|---|---|
| login, PeerInfo ("Linux"), the desktop in the right colours (gamma) | works |
| pointer, clicks, keys (Map: X11 keycodes; Legacy: characters), typing into a winterm | works |
| menus (Toolchest, 4Dwm, Motif) -- they are in the 2-bit overlay planes | works |
| scrolls and window moves as motion vectors | works |
| clipboard both ways (CLIPBOARD and PRIMARY; zstd from the peer; Latin-1 here) | works |
| file manager beside the desktop: list, get, put, mkdir, rename (byte-identical round trip) | works |
| RustDesk 1.4.9's own file manager window: both panes listed (once PeerInfo carried a username) | works |
| a remote logout (Toolchest > Desktop > Log Out), the X server reset, the agent reconnecting, and a remote login typed into clogin | works |
| the package: IRIX tar unpacks it, its scripts pass IRIX `sh -n`, `-i` makes an identity | works |

**A real RustDesk client**: RustDesk 1.4.9 on this Linux machine
(`rustdesk --connect 192.168.99.38 --password ...`, its window captured with
`xwd -id`): the picture, a click, and typing a shell line in Map mode, shifted
symbols included. It asks for 30 fps (`custom_fps`).

**By ID through the user's own server**, with that real client: the
agent registered with hbbs, and both the local-address path (hbbs chose it:
the client is on the same LAN) and the relay (`rustdesk --connect ID/r`)
gave encrypted sessions. The Indigo has **no default route and no DNS** -- it
reaches 192.168.99.0/24 only -- so this went through a temporary forwarder on
this Linux machine (a few lines of Python asyncio: LAN 192.168.99.153 UDP+TCP
31116 -> hbbs 21116, TCP 31117 -> hbbr 21117, with `server=` and `relay=` in
the test prefs pointing at it); it was stopped afterwards and the prefs put
back. Giving the Indigo a route is the user's call.

**Since then (second session, 2026-10-02)**: installed for good (inst
product, boot start, a settings panel in the Toolchest), on the network by
name (the Indigo now has a route, DNS and a right clock), listed in the
user's console over https, screens of any depth chosen at run time (the
login pane's IRIS GL window was wrong until then), and released: build
20261002-7729fb115 in `r-deskway-1.5.0` (see section 7).

**Not proven**: any 24-bit SGI (only Xvfb at depth 24 as a stand-in);
screens larger than 1024x768; any IRIX but 5.3.

## 2. The build: GCC objects, SGI's linker

IRIX 5.3 has IDO 3.19 (C89) and no newer compiler; the shared core is C99.

- **GCC 13 `mips-linux-gnu`** (Ubuntu's cross packages, unpacked without
  root: `scripts/fetch-toolchain.sh`) compiles everything with `-march=mips1
  -mabi=32 -mabicalls -fPIC -mno-relax-pic-calls -G0 -Wa,-mdebug`, against
  IRIX 5.3's own headers (`build/irix53-sysroot`, out of
  `~/Indy-IRIX53_dev.chd` with `rb-cli tar`; licensed, never committed), with
  IDO's predefines (`cc -v` printed them; they are in `tools/irix-gcc`).
  GCC schedules MIPS I's load and multiply delay slots; LLVM does not.
- **`tools/asbin/as`** wraps GNU as (found through `-B`): GCC files PIC data
  with relocations under `.data.rel*`, constants under `.rodata.str*`; IRIX
  5.3's ld *asserts* on names it does not know (`obj_elf.c`, line 353), so
  they are renamed in the assembly to `.data`/`.rodata`/`.text`.
- **`tools/irixobj.py`** after every compile (see its header):
  drops `.MIPS.abiflags`, `.gnu.attributes`, `.note.GNU-stack`, `.pdr`,
  `.comment`, `.mdebug.abi32`; clears `EF_MIPS_ABI_O32`; **rebases `.mdebug`**
  (its symbolic header holds absolute file offsets, which objcopy does not
  move); and **reshapes the symbol tables like IDO's**: ELF locals are only
  section symbols (static functions live in `.mdebug` alone), and `.mdebug`'s
  externals pair one-for-one, in order, with the ELF globals -- gas lists some
  statics there, which ld reports as "bad index in get_extmap()" or
  "external index is N but there are only N". `tools/mdebug-dump.py` shows
  both tables.
- **libgcc is not linked** (Ubuntu's is MIPS32r2): `src/compat.c` has the
  helpers MIPS I needs (`__bswapsi2`, `__clzsi2`, 64-bit division...) and an
  `snprintf` (IRIX 5.3's libc has none). `utimes` is in libc, not in a
  header (`compat.h` declares it).
- **SGI's ld links it on the Indigo**: `scripts/indigo.py link` uploads what
  changed, runs `build/obj/LINK` there (`cc ... -lXTrap -lXt -lXtst -lXext
  -lX11 -lm`) and fetches `build/C-Desk-Vint`. `make IRIX_LDFLAGS=-p` links
  for `prof` (pc sampling works on these objects, function by function).
- `make dist` packs the linked binary with `pkg/` into
  `build/C-Desk-Vint-IRIX-5.3+-<build>.tar` (v7 tar) and SHA256SUMS.

## 3. The design, and why

**Reading the screen is the expensive part.** LG1 gives pixels back at
~3.5 Mpixel/s through MIT-SHM: a full 1024x768 read is 234 ms, and every
request costs ~2.1 ms on top (XGetImage over the socket: 1085 ms). So
`src/screen.c` reads only what changed:

- **DEC-XTRAP** reports every drawing request any client makes, with the
  drawable's absolute position (`XETrapWinXY`); the request's own coordinates
  give the rectangle. Pixmaps are tracked (CreatePixmap/FreePixmap, and a
  BadWindow probe for ones made before we started) and ignored. CopyArea's
  destination is not the drawable XTrap positions: a small cache of window
  origins, filled from the reports, answers it.
- **SubstructureNotify** on the root: top-levels mapping, moving, going --
  when the server paints without being asked.
- A **rolling band** (3 macroblock rows every 150 ms) catches what X never
  sees (IRIS GL, the server's own painting); it slows to one band a 1.2 s
  after a pass that found nothing X had not reported, and speeds up again
  as soon as one does.
- **Colormap** requests are trapped: on an 8-bit screen a colormap change
  recolours pixels that never change value. The default colormap is preferred
  when it is installed (the installed list's order changes with focus).
- Damage is merged into few rectangles (a request costs ~7000 pixels).
- **Overlays**: menus (Toolchest, 4Dwm, Motif popups) are 2-bit windows in
  the popup planes (`SERVER_OVERLAY_VISUALS`: visual 0x25, transparent pixel
  0), invisible to a read of the root. Their own pixels are read (XShmGetImage
  of the overlay window, depth 2) where something drew on them, and laid over
  the screen per pixel when converting. The desktop's `FmBackground` is a
  full-screen, almost wholly transparent overlay: it is read only where drawn
  on, and only macroblocks where an overlay shows count as overlaid.

**Encoding** is the shared VP8-lite (`../crustdesk-macos-classic/src/core/
vp8enc.c`), grown for this port (see its header):

- **Motion vectors**: XTrap's CopyArea between on-screen windows (a scroll)
  and a moved top-level become per-macroblock hints; two scrolls in one frame
  compose. A moved block that differs only by the coding error it already
  had is sent as the vector alone. Bit-exact with libvpx
  (`host/test_mv.c`, odd and even shifts, both directions).
- **A frame budget** (`src/main.c`, `select_mbs`): a frame codes what fits in
  ~220 ms of this CPU -- flat blocks are cheap, moved ones nearly so, detail
  is learnt from measured frames (~2.7 ms a macroblock) -- longest-waiting
  first, then rows nearest the pointer; the rest stays dirty for the next
  frame. The first picture (and a client's refresh) is a keyframe of the
  background colour (~0.9 s) and the real picture paints in at 2.5-3 fps.
- Encoding runs 4 macroblock rows at a time with the network serviced in
  between, so input keeps flowing during a long frame.

**Input**: XTEST, after `../rustdesk-irix65-agent/src/input_shim.c` --
keysyms, not keycodes, so the Indigo's layout decides; a character no key
makes borrows a spare keycode. We say "Linux", so Map mode sends X11/evdev
keycodes (`../rustdesk-ppc-agent/src/linux_keycodes.h`).

**Two session slots**: a desktop (owns the screen and the encoder) and a file
manager beside it (`host/fs_posix.c` with root "" -- the whole file system,
".." refused).

**X lifecycle**: the agent starts without a display and opens it every 5 s
until it can; an Xlib I/O error (a logout resets the server: xdm here has
`terminateServer: False`) longjmps to the main loop, which forgets the
connection *before* ending the session (or the teardown's X calls would fail
back into the handler), closes the socket without XCloseDisplay, and
reconnects. `authorize: off` in xdm-config: root opens `:0` without a cookie.

## 4. Measured, on the Indigo

| | |
|---|---|
| full-screen readback, MIT-SHM | 234 ms (a request: ~2.1 ms + 285 ns/pixel) |
| first picture (flat keyframe) / whole desktop painted in | ~0.9 s / ~6-7 s at 2.5-3 fps |
| a small change (clock tick, a keystroke) | 40-70 ms a frame, 200-400 bytes |
| a menu opening (~136 macroblocks) | ~0.37 s |
| an 80x30 xterm scrolling one line (encbench) | 0.66 s, 6 KB (was 2.2 s, 86 KB) |
| a detailed (text) macroblock, coded | ~2.7 ms, nearly independent of q |
| fixed cost of any frame (3072 macroblocks' modes) | ~27 ms (was ~40: see "write-through" in section 5) |
| background band scan, idle | ~0.3 s of reading a 10 s (adaptive; was 1.4 s) |
| file transfer, download / upload | 584 KB/s / 730 KB/s |

The Indigo's one CPU is shared with Xsgi and the programs: during a fast
`ls` frames take 1-2 s of wall time for 0.4-0.7 s of encoding.

**The soak** (cdvpoke, ~4 minutes: typing `ls -l /usr/lib` into a winterm,
opening and closing the Toolchest's menu, moving the pointer, 14 rounds):
with the frame budget at 220 ms every 10-second window of the agent's log
was at 2.1-3.5 fps (573 frames, 760 KB); at 300 ms it was 1.6-2.8. The
agent's size after two soaks was the same (3934 pages, 1818 resident: 15 MB and 7 MB):
nothing leaks.

## 5. Traps

- **The terminal line is 255 bytes** (canonical mode): a longer command is
  truncated and the shell waits for the rest of an unbalanced line, forever.
  `scripts/indigo.py` sends long commands as a script file.
- **Typing files through telnet loses characters**: IRIX's pty discards input
  once its queue is full (`-imaxbel`). Only `tools/irix/xfer.c` is typed;
  everything else moves over TCP with it.
- **IRIX 5.3's grep has no `\|`**; use egrep. `tr "\n"` is a literal n.
  `uudecode` has no `-p`. `/etc/killall NAME` is fine (it takes names).
- **The R3000's 32 KB direct-mapped data cache**: buffers whose sizes are
  multiples of 32 KB, read in step (the screen, the luma plane, the SHM
  segment), alias line for line; comparing two such 768 KB buffers took
  231 ms. The arena in `screen_open` staggers them.
- **`XETrapDispatchEvent` hands every non-XTrap event to XtDispatchEvent**,
  and Xt exits ("Couldn't find per display information") on a display it did
  not open. Dispatch XTrap's own event type with `XETrapDispatchXLib`.
- **XTrap needs `NEED_EVENTS`/`NEED_REPLIES` before its headers**, and
  `-I.../X11` (it includes "Xlib.h" bare). XShmQueryExtension is in SGI's
  libXext but not its XShm.h.
- **`XListInstalledColormaps` lists the overlay maps too** (2 and 4 bits);
  asking one for 256 colours is BadValue, which killed the first run.
- **A telnet login exports `DISPLAY=<your host>:0`**; programs started from it
  must be given `:0`. A program started with `&` dies with the session
  (SIGHUP) unless it ignores it (the agent does); `nohup` made the helper's
  shell wait. `xterm` with stdin from /dev/null exits at once.
- **The R3000's data cache is write-through**: every store goes to memory
  through a four-entry write buffer, so a loop that stores a lot stalls
  however little it computes. Skipping 3072 macroblocks cost ~23 ms until
  the skip path stopped rewriting zeros; the bool coder in `put_modes` now
  works on a local copy (registers) as `put_block` always did.
- **`gettimeofday` is a real system call**: VP8E_PROFILE's per-macroblock
  clock reads cost more than what they measured. Use `prof`.
- **Wrapping millisecond clock**: a deadline initialised to 0 compares as
  far in the future; initialise deadlines from the clock.
- **The Indigo has no default route or DNS** (`netstat -rn`); only the LAN.
- **cdvpoke's upload inherited a 2 s timeout** from its last receive; a 128
  KB block to this machine takes longer (fixed in cdvpoke).
- `pkill -f` matching your own command line kills the Bash tool's shell.
- **A boot empties /tmp**: `/etc/init.d/filesystems` moves it to `/.oldtmp`
  and `rmtmpfiles` deletes that, unless `chkconfig nocleantmp` is on. The
  user's `/tmp/t.c`, `/tmp/t1`, `/tmp/t2` were lost that way to the first
  reboot of 2026-10-02. Before rebooting, look in /tmp and ask.
- **Relinking a running agent kills it** (SIGBUS, an instruction fetch from
  a page of the rewritten file): stop it first. The core lands in `/core`,
  and inst moves it to `/usr/tmp/core.postinst_detected`. IRIX's `dbx` will
  not start while the machine's own name does not resolve; the core's
  header (`core.out.h`) gives the signal, EPC and BADVADDR.

## 6. Environment

- The Indigo: `192.168.99.38`, telnet as root, no password (the user gave
  it). `scripts/indigo.py run|put|get|spawn|link|linkobj` (env `INDIGO`).
  Development builds live in `/usr/tmp/cdv-build` (root has ~10 MB free;
  /usr has GBs); the test agent runs from there with `/usr/tmp/cdv-prefs`
  (`server=` empty, so no registration) via `scripts/restart-agent.sh`;
  `scripts/deploy.sh` = make + link + restart. `/usr/tmp/bg PROG ARGS`
  starts an X program detached on `:0`. `/usr/tmp/cdv-xfer/xfer` is the
  transfer helper.
- The test peer: `../crustdesk-macos-classic/host/cdvpoke.py 192.168.99.38:21118
  <password> ...` (`save FILE`, then `host/build/vp8dump FILE out.ppm`).
- `bench/encbench.c`: the encoder alone on the target (`grab` a screen,
  `run` it with a scroll, `NOHINTS=1` for no vectors); `make -C bench
  NOINLINE=1` keeps functions apart for `prof`.
- **Nothing of the build is kept locally** (the disk is nearly full): set it
  up again with `scripts/fetch-toolchain.sh` (into `build/toolchain`, ~1
  minute) and `IRIX53_IMAGE=~/Indy-IRIX53_dev.chd scripts/make-sysroot.sh`
  (~30 s); RFC 6386 for `gen_vp8_tables.py` is at
  https://www.rfc-editor.org/rfc/rfc6386.txt. The released-quality package
  of build 20261002-f2609ae03 is kept in `~/vintage-state/crustdesk-irix/`.
- **On the Indigo now**: the release build, installed by `inst` (product
  `cdeskvint`, version 2026100285), started at boot, with the identity in
  `/var/C-Desk-Vint/prefs` (ID 666775149) pointed at the user's own server
  (rustdesk.home.dani.tech, its key) and console (`api=https://
  remote.home.dani.tech`) -- personal settings, in that file only. The
  development copy and test prefs (`/usr/tmp/cdv-build`, `cdv-prefs`, a
  second identity, ID 839891312) are kept for the next session; `inst_dev.sw`
  (gendist) is installed from `/CDROM2/dist`. The user's `/tmp/t.c`,
  `/tmp/t1`, `/tmp/t2` are gone (a boot empties /tmp: section 5).
- **The Indigo's network**: default route 192.168.99.1, DNS 192.168.99.1;
  the clock was set right by the user (5.3 has no ntpdate).
- **Driving it**: `scripts/indigo.py inst DIST 'cmd'...` drives inst (it
  discards typing while it reads its history -- minutes here); the scripted
  peer (`cdvpoke.py ... save F`, then `vp8dump F out.ppm`) clicks at exact
  screen coordinates, which the real client's scrolled view does not;
  `endsession -f` with DISPLAY=:0 logs the desktop out; a reboot is
  `(trap "" 1; sleep 3; /etc/shutdown -y -g0 -i6) &` (`/etc/reboot` asks).

## 7. Done in the second session (2026-10-02), and what is left

Every item of the work list, in the order of the resume prompt:

1. **Boot start**: `install.sh` ran; chkconfig `cdeskvint on`; it starts at
   boot, registers and serves the login screen (checked after reboots, the
   last with the release build).
2. **inst**: `inst/cdeskvint.{spec,idb}`, `scripts/make-inst.sh` runs the
   Indigo's gendist; `make dist` = tar + tardist + SHA256SUMS. gendist
   strips the executable. **5.3's gendist has no removeop** (drops it
   silently: only preop/postop/exitop), so the agent, as a `-D` daemon run
   by full path, stops by itself within 10 s of its file being removed.
3. **Mac username "none"**: done; RustDesk 1.4.9's file manager opens the
   Quadra 800 in QEMU. Not released for the Mac yet (its release is still
   6de1828f9).
4. **DNS and https**: `src/report.c` -- a forked child at nice 10 (at the
   same priority its handshake starved the agent and a local connection's
   client gave up after 1 s), BearSSL from the shared core, backoff 15 s to
   5 min. The user's nginx closes a TLS handshake not done in 10 s; two
   P-384 checks (~4.5 s each on the R3000, `bench/tlsbench.c`) missed it, so
   Let's Encrypt's E5-E9/YE1-YE3 are anchors now (shared core, Mac too): ~7 s.
   Under load (just after boot IRIX runs `man -W` for minutes) listing waits.
   The ID server is looked up by a child down a pipe, and again after 90 s
   without an answer. `cdv_ms()` absorbs clock steps (setting the clock back
   3 h had stalled the 30 s retry for 3 h).
5. **Any depth, run time** (`capture=` auto|8|24|readdisplay): an 8-bit or
   24-bit root through SHM (the socket without SHM), plus the hybrid -- the
   Entry board keeps 7 colormaps installed, and clogin's icon pane is an
   8-bit TrueColor GL window with its own colormap; top-levels are walked for
   windows of another visual/colormap and their changed macroblocks re-read
   through ReadDisplay (~4 ms each, 64 a frame). ReadDisplay's pixels are
   ABGR. Checked: the Indigo (auto, 8, readdisplay) and Xvfb depth 24 over
   TCP (little-endian server, no SHM). **No 24-bit SGI yet** -- the user
   said not now to O2 access.
6. **Pointer**: `CDV_TRACE_CURSOR=1` logs cursor requests. A Toolchest
   launch: its menu grabs the pointer with the cursor font's `arrow`
   (black/white; IRIX's own is a red `left_ptr`) until the program's window
   maps (26 s for winterm); nothing else changes the cursor, no extension
   request either. Sending shapes is worth it in part: grabs are easy to
   follow through XTrap and cover this, menus and 4Dwm moves/resizes; the
   cursor font's glyphs would have to be carried (~10 KB). Window cursors
   are hard: X cannot be asked for one, and CreateWindow's cursor is past
   the 48 bytes XTrap hands over. Not done: the user to decide.
7. **Release**: plus, at the user's request mid-session, **a settings panel**
   (`gui/panel.c`, from the 6.5 agent's Motif panel; `pkg/cdv-helper.sh`;
   `pkg/C-Desk-Vint.chest`), which found a real bug (a daemon kept the
   helper's pipe: `daemonize()` now closes inherited descriptors). Released
   build **20261002-7729fb115** (tar, tardist, SHA256SUMS, MANIFEST.txt) in
   `r-deskway-1.5.0`, with a row in both tables and its own section.

8. **The pointer, after the release** (the user, from RustDesk 1.4.9 on a
   Mac: "the mouse is drawn so slowly that it's impossible to track"): the
   agent sent the pointer's position whenever it changed, the client's own
   moves included. The client (flutter/lib/models/model.dart,
   updateCursorPosition; input_model.dart, _checkPeerControlProtected) takes
   every CursorPosition as the remote side moving the pointer: it hides its
   own, draws ours, and ignores its mouse for kMouseControlTimeoutMSec (1 s).
   Now nothing is sent within 300 ms of the peer's input (RustDesk's own
   server does that, input_service.rs run_pos) nor while the pointer is where
   the peer put it. The user found it usable; build 20261002-7729fb115
   replaced a0054ba77 in the release. Tell the client nothing it did itself.
9. **"this machine's screen"**: the refusal of a second desktop session (in
   the shared core) said "this Mac's screen"; 20261002-7729fb115 replaced
   d061a0d3a in the release for it. That refusal also covers IRIX's "no X
   display yet" and "no memory for the screen", where its words are wrong.
   And a session the client dropped keeps the screen until TCP notices (two
   minutes once), so a quick reconnect is refused: worth a timeout.

Still open: cursor shapes (above); a 24-bit SGI to test on; speed for coded
text (~2.7 ms a macroblock on the R3000) and a pass that sharpens what was
sent coarsely; the console under load; Stop/Start pressed in the panel.
