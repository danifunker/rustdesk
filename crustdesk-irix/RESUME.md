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
| a remote logout (Toolchest > Desktop > Log Out), the X server reset, the agent reconnecting, and a remote login typed into clogin | works |
| the package: IRIX tar unpacks it, its scripts pass IRIX `sh -n`, `-i` makes an identity | works |

**Not proven**: a real RustDesk client (only cdvpoke has driven it -- the
core was proven against RustDesk 1.4.9 on the Mac); the ID server, relay or
local-address path (the Indigo has **no default route and no DNS** -- it
reaches 192.168.99.0/24 only, so neither the user's server nor the public one
is reachable; changing that is the user's call); `install.sh` actually run
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
  sees (IRIS GL, the server's own painting).
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
  ~300 ms of this CPU -- flat blocks are cheap, moved ones nearly so, detail
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
| fixed cost of any frame (3072 macroblocks' modes) | ~40 ms |
| file transfer, download / upload | 584 KB/s / 730 KB/s |

The Indigo's one CPU is shared with Xsgi and the programs: during a fast
`ls` frames take 1-2 s of wall time for 0.4-0.7 s of encoding.

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
- The cross toolchain was unpacked in `/tmp/mipsgcc` this session and
  `build/toolchain/usr` links to it; `scripts/fetch-toolchain.sh` makes a
  fresh one. RFC 6386's text (for `gen_vp8_tables.py`) was fetched to
  `/tmp/rfc6386.txt`.
- Left on the Indigo by testing: an xterm and an xclock in the restored
  session (`/etc/killall xclock xterm` if unwanted), `/usr/tmp/{cdv-build,
  cdv-bench,cdv-xfer,pkgtest,moved.bin,cdvtest,xterm.scr,...}`, and probes in
  `/tmp` and `/usr/tmp`.

## 7. Next

- A real RustDesk client against it (by IP on the LAN today).
- The ID server, once the Indigo has a route out (or a LAN-side hbbs).
- Speed: the ~40 ms a frame for 3072 skipped macroblocks' modes; coded text
  at ~2.7 ms a macroblock (tokens ~20%, the rest spread over prediction,
  transforms and reconstruction); a refresh pass that sharpens what was sent
  coarsely.
- 24-bit displays (Indy XL24, Indigo2): `yuv.c` already converts 32-bit; the
  readback and comparison are 8-bit only.
- IRIX's own pointer shapes (no XFixes: perhaps from XTrap's DefineCursor
  requests).
