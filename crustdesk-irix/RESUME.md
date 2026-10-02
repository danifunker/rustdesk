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

**Not proven**: `install.sh` actually run
(boot start, chkconfig) -- it was not, because it changes the machine's rc
configuration; 24-bit displays (unsupported: the code refuses a root window
that is not 8 bits deep).

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
- **On the Indigo now**: that build running from `/usr/tmp/cdv-build` with the
  test prefs `/usr/tmp/cdv-prefs` (no ID server: direct IP on port 21118;
  `C-Desk-Vint -i -c /usr/tmp/cdv-prefs` prints its ID and password), the log
  in `/usr/tmp/cdv.log`, and `/usr/tmp/cdv-xfer`. It does not survive a
  reboot: `install.sh` was not run. Everything else testing made was removed;
  `/tmp/t.c`, `/tmp/t1`, `/tmp/t2` predate this work and are the user's.

## 7. Next: the user's decisions of 2026-10-02

The user's answers to the first round (the order is RESUME-PROMPT.md's):

1. **Higher-powered SGIs (24-bit) must be supported, adjustable at run time.**
   Today the 8-bit limit is code, not a setting: `screen_open` refuses a root
   that is not 8 bits deep (`src/screen.c`, "only 8 is supported yet"), every
   readback, compare and copy is one byte a pixel, conversion goes through the
   colormap tables, and `MAX_W`/`MAX_H` (1280x1024, `src/main.c`) size the
   buffers at compile time. What it takes:
   - Detect the depth and pixel layout (the visual's masks -- SGI's 24-bit
     TrueColor is not necessarily xRGB; `yuv.c`'s 32-bit path assumes xRGB
     big-endian), read 4 bytes a pixel, compare and convert from that; a prefs
     setting (say `capture=auto|8|24|readdisplay`) to override.
   - **Mixed depths**: on Indigo2/Indy 24-bit boards the root is often an
     8-bit PseudoColor with 24-bit TrueColor windows over it; a read of the
     root is undefined where a window of another depth shows. SGI's
     **ReadDisplay** extension returns the composited picture -- probed on the
     Indigo 2026-10-01: `XReadDisplay(dpy, root, x, y, w, h, 0, &ret)` gave
     32 bpp RGB (0xd9d9d9 for a grey menu) and raised an X error 129 that did
     no harm. It goes over the socket (no SHM), so it is slower a pixel.
   - Allocate from the real screen size at login, not MAX_W/MAX_H.
   - The binary (MIPS I, o32) runs on IRIX 6.5 too, so the user's O2 (24-bit,
     6.5) could test it -- ask for access; nothing here has run on 24 bits.
2. **The pointer** stays a plain arrow, but the documents must say so (done:
   `README.md`, `pkg/README.txt`). Open question from the user: **what does
   the client see while IRIX launches a program and the pointer flashes?**
   Nothing: the client draws the arrow we sent once, at the position we send
   every 100 ms. Find out how IRIX makes that feedback (XTrap log of
   CreateGlyphCursor 94, CreateCursor 93, ChangeWindowAttributes with
   CWCursor, GrabPointer 26, ChangeActivePointerGrab 30, RecolorCursor 96,
   while launching from the Toolchest) and decide whether to send cursor
   shapes (cdv_send_cursor takes RGBA; glyph cursors would need the cursor
   font's bitmaps).
3. **The file-manager username.** IRIX already sends the user logged in on
   `:0` (utmp; `console_user()` in `src/main.c`), root at the login screen --
   the agent runs as root but reports the desktop's user, which is what the
   client means. **The Mac must send "none"** (the user's choice): set
   `ident.username = "none"` in `../crustdesk-macos-classic/src/mac/main.c`
   where `ident` is filled (~line 2009); any non-empty name satisfies the
   client. Test with a real client's file manager if one can be driven
   (cdvpoke ignores PeerInfo), at least in QEMU (`scripts/q800.sh`).
4. **Networking** is the user's to finish (a default route via 192.168.99.1
   and DNS). They want the agent to work by name over DNS and over **https**:
   - DNS: the agent resolves with `gethostbyname` (IRIX's resolver,
     `/etc/resolv.conf`); test `server=` by name once it is set up (the public
     `rs-ny.rustdesk.com` and the user's own). The core's own DNS client
     (`src/core/dns.c`, the Mac's fallback) is not built here.
   - https: **not built on IRIX yet.** The core has the console client
     (`console.c`, `https.c`, BearSSL in `third_party/bearssl`, ISRG anchors
     from `tools/gen-anchors.py`): `/api/sysinfo` and the 15 s
     `/api/heartbeat`, which put the machine in a RustDesk console's device
     list. A TLS handshake took ~11 s on a 68040; on the R3000 it would stall
     sessions if done in the main loop -- run it in a forked child with a pipe.
     `api=` in prefs, empty by default (generic builds carry no console; the
     user's is `https://remote.home.dani.tech`, for their own prefs only).
     Certificate dates need the Indigo's clock (it is right today).
5. **Boot start through chkconfig: approved.** `pkg/install.sh` already
   copies the rc script, links S99/K01 and runs `chkconfig -f cdeskvint on`;
   it has never been run. Run it on the Indigo (stop the development agent in
   /usr/tmp first; the installed one gets its own identity in
   `/var/C-Desk-Vint/prefs`), check `chkconfig` lists `cdeskvint on`, reboot
   and check it registers and serves at the login screen.
6. **inst packaging, then the release.** The user asked whether IRIX 5.3's
   `inst` can install it: not as it stands (a tar and install.sh). It can be
   made so:
   - `gendist` is in **`inst_dev.sw`**, not installed on the Indigo but on its
     own CD: `/CDROM2/dist/inst_dev.*` (`inst -f /CDROM2/dist`, pick
     inst_dev.sw). The user's **irixscsitb** already makes 5.3 inst images
     with the guest's own gendist and ships them as `/dist53` on its CD (the
     Indigo's `/CDROM` is that CD): `~/repos/irixscsitb/scripts/iris-gendist.sh`,
     `docs/ci-iris.md` ("Driving the guest" lessons), `inst/README.md`. The
     IRIX 6.5 agent's templates are `../rustdesk-irix65-agent/inst/*.spec,*.idb`
     (the idb must stay sorted by destination path).
   - The product: /usr/local/sbin/C-Desk-Vint, /etc/init.d/cdeskvint and its
     rc links, the chkconfig flag file (`config(noupdate)`), the read-me; an
     exitop that turns chkconfig on and starts it, a removeop that stops it.
   - **Release**: the user wants it in the version matrix of the
     `r-deskway-1.5.0` release, like the other vintage builds (see the memory
     notes: assets named for the platform, `C-Desk-Vint-IRIX-5.3+-<build>.*`;
     notes are CRLF; say exactly what was tested and where). Release the build
     that was installed and run on the Indigo; ask before uploading.

Still open from before: speed for coded text (~2.7 ms a macroblock on the
R3000) and a refresh pass that sharpens what was sent coarsely.
