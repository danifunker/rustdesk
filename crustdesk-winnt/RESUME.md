# RESUME — C-Desk-Vint for Windows NT

State of the Windows NT 4.0 port, how it is built and tested, what is proven
and what is not, and the traps that cost time the first time. This is the
source of truth; `RESUME-PROMPT.md` points here; `README.md` explains the
layout and the build; `pkg/README.txt` is the user's read-me.

---

## 1. Where it stands

Started 2026-10-29 on `vintage-agents`. The user asked for an NT 4.0 agent
"which can be compiled for MIPS/Alpha/PowerPC/x86", PowerPC first, with the
features of the IRIX and Mac agents, running as a service with a window to
manage it, reachable over the internet.

C-Desk-Vint (the Mac agent's name: a C program on the Mac agent's portable
core) is one `C-Desk-Vint.exe`: the settings window with no arguments, the
service with `-service`, a console agent with `-run`, and `-i`, `-install`,
`-remove`, `-start`, `-stop`, `-selftest`. It reports itself to the peer as
"Windows", so a client's Map mode sends Windows scancodes and its file
manager shows drives.

Proven on **QEMU's IBM 40P running NT 4.0 SP2 for PowerPC** (section 5),
built there by Visual C++ 4 (`cl` 10.10), every frame decoded by libvpx
(`../crustdesk-macos-classic/host/build/vp8dump`):

| | |
|---|---|
| `-selftest`: SHA-256, Ed25519, box, secretbox, zstd, PNG, VP8 all bit-identical to GCC's build (and the crypto to libsodium's) | works |
| registering with rs-ny.rustdesk.com from behind QEMU's NAT | works |
| **by ID, over the internet**: `rdvpoke.py` asks rs-ny for the ID, the agent goes to the public relay (ovh-east2.rustdesk.com), key exchange, encrypted session, video | works |
| by address (direct port 21118), with **RustDesk 1.5.0's own client** on this Linux machine (`rustdesk --connect 127.0.0.1:21118 --password ...`): login, 30 fps asked, a 70 s session | works (the agent's log only: GNOME refused a screenshot of the client window) |
| the desktop in the right colours at 1280x1024, the pointer's real shapes (arrow, I-beam) and position | works |
| typing into Notepad: Legacy characters, Map-mode scancodes with Shift, accents by Alt+0nnn (`ü é ñ`) | works (after pacing, section 4) |
| clicks: activating a window, opening a menu | works |
| clipboard both ways, Unicode (`☺`, `Grüße`), zstd-compressed from the peer | works |
| file manager: drives at "/", list, get, put (300 KB byte-identical round trip), mkdir (Unicode name), rename, rm, rmdir | works |
| screenshot (PNG, 1280x1024) | works |
| the settings window: Install copies to Program Files, makes the service, starts it, a Start Menu entry, reopens from the installed copy; status "Ready: registered...", "Connected: desktop cdvpoke (relayed)" | works |
| **the service as SYSTEM**: Ctrl-Alt-Del from the peer (`ctrl 100`) brings up Windows NT Security; the agent follows to the Winlogon desktop; Lock Workstation, the locked screen, Ctrl-Alt-Del, the Unlock dialog, typing the password, back to the desktop | works |
| the console reporter's https (BearSSL by MSVC 4) against a Let's Encrypt site: verified handshake, an HTTP answer (405), inventory "PowerPC; 128 MB; Windows NT 4.0 (build 1381, Service Pack 2) PowerPC" | works (no real console tried) |

**x86 (second session, 2026-10-30)**, on QEMU's i440FX PC (Pentium, TCG)
with NT 4.0 Workstation (no service pack), built there by Visual C++ 4.0
for x86 (`cl` 10.00.5270): `-selftest` all pass; installed as a service
(`-install`), registered with rs-ny, a session **by ID through the public
relay**, typing (Legacy, Map with Shift, Alt-codes `ü é ñ`), clipboard both
ways (Unicode), files (200 KB round trip, a Unicode folder), Ctrl-Alt-Del
to Windows NT Security from the service. The PowerPC machine was then
pointed at the user's own servers (its settings file only): registered with
their ID server, **in their console's device list over https** (their
Let's Encrypt chain verified), a session by ID through their relay.

**Windows 98 SE** (2026-10-30, the released x86 .exe unchanged), on the
user's own VMware Windows 98 install (`smb://danihtpc.local/temp/Win98VM`,
a 98QuickInstall with Visual Studio 6, Borland C++ 5 and the 98 DDK)
under QEMU (`scripts/qemu-win98.sh`): selftest; Install from the settings
window (Program Files, RunServices, Start Menu); a session by ID through
the public relay with `scripts/session-test.sh` (typing, accents, the
clipboard in the code page -- "☺" became "?" as designed --, files, a
screenshot); `-stop`/`-start`; started at a cold boot before logon and
served the logon prompt; a remote logon; still serving after a logoff.
Build 20261030-feaa6be71 then fixed the settings window (two columns,
206 dialog units: whole on 640x480; titled for the Windows it is on) and
passed the same tests on all three machines. `-start`/`-stop` run with no
console answer "done" in a message box that waits for OK -- such a helper
holds the program file, so a file swap on 9x needs it dismissed (or
`tools/nt/plist.c kill`). **Windows 95**: loads (check-imports.py against
95 RTM's DLLs), never run.

**ARM: PINT, NT 4.0 on the Raspberry Pi** (2026-11-02, build
20261102-9e35e4ebe). No compiler ran on NT for ARM, so this build is made by
PINT's own build on a GitHub ARM runner (`winnt-pint-agent-build.yaml`,
`scripts/pint-build.sh`, `scripts/stage-pint.py`: the agent as a PINT app,
Visual C++ 4.2's libcmt and the import libraries built from the NT 4.0 tree;
~15 minutes from nothing). Every import checked against the card's DLLs
(`check-imports.py`). On a PINT card (pint40-2026-11-02b) in QEMU raspi2b
(`scripts/qemu-pint.sh`), four processors: `ntexec.exe` of the same build
served `ntx.py`; `-selftest` all pass; `-install` as a service, registered
with rs-ny; **a session by ID through the public relay** (ovh-east2):
typing into Notepad, Legacy and Map mode with Shift ("NT4"), a 1920x1080
screenshot. PINT's 58 edit scripts over NT 3.5 and XP SP1 files are left
out (those trees are not fetched; nothing the agent links). Not yet: the
clipboard, files, Ctrl-Alt-Del, the settings window, a real Pi;
`cliptool.c` is not built for ARM.

**Released** 2026-11-04 as `vintage-winnt-arm-20261104-948ae0216` (its own
series: the wiki's tools/release.py has a row for it), build
20261104-948ae0216 from the workflow's run 41820870360: that .exe, put in
place of the service on the same PINT machine, passed -selftest and the
session by ID through the public relay again. The release files are the
run's artifact plus a MANIFEST.txt written by hand.

**Alpha** (2026-11-05, build 20261105-e32184f8b): the staged tree built
unchanged by Visual C++ 6.0 on NT 4.0 for Alpha (`nmake` in `build\stage`;
the only change it needed was CRLF in the staged sources, for NT's editors).
Tested by the user on NT 4.0 for Alpha: a full session by ID through their
own RustDesk server, copy and paste, file transfer, a screenshot. Not tried:
Ctrl+Alt+Del from the client, `-selftest`. Sessions felt slow; not measured.
**Released** 2026-11-05 as `vintage-winnt-alpha-20261105-e32184f8b` (its own
release, like ARM; the user asked that the notes say only "NT 4.0 for Alpha,
tested", not how). Release files: `make dist ARCH=alpha` plus a MANIFEST.txt
written by hand. The wiki's Downloads page has no row for it yet.

**Not proven**: the MIPS build (not compiled: no NT machine or compiler for
it yet; nothing in the source is processor-specific, and `-selftest` exists
to check it); real PowerPC hardware; NT 4.0 with other
service packs; a real RustDesk console; a screen depth other than the 40P's
(GDI converts any depth into the 32-bit DIB, so it should not matter);
`restart` from a peer.

**Since the Alpha release** (2026-11-05). Built by Visual C++ 6.0 on the
Alpha as test build 20261105-848360c16-tray (sha256 6225efad...; a copy in
build/alpha/, not released); the user reports **the tray, its chat window
and Ctrl+Alt+Del working** there. Not reported yet: Disconnect from the
tray, the two-line File access hint, Windows 2000's lock and block input.

- *Ctrl+Alt+Del in the client's menu.* A client offers "Insert Ctrl + Alt +
  Del" only for a peer whose PeerInfo has `sas_enabled` (field 6) or whose
  platform is "Linux"; the core never sent it, so no client showed it --
  the earlier tests sent the key from the scripted client. Now
  `cdv_ident.sas_enabled`, set by the NT service (not -run, not 9x).
- *The tray and chat* (`-tray`, tray.c; link.c). The service is SYSTEM, so
  chat is shown by a program of the logged-on user's, started from
  HKLM\...\Run (written by service_install, removed by service_remove;
  Install also starts it at once). They talk over the named pipe
  `\\.\pipe\C-Desk-Vint`, message mode: a type byte, then UTF-8 (link.h).
  The pipe's DACL lets Everyone read and write it but only SYSTEM make
  instances (so no program can pose as the agent while it runs); after the
  tray's hello the agent impersonates it and lets it in only if its SID is
  the owner of Progman's process -- the console user (refreshed every 2 s).
  A refused tray is told why and waits a minute before trying again. Chat
  from the peer with no tray: the peer is told once a session. The tray:
  Chat (opens by itself on a message), Disconnect remote sessions,
  Settings, Exit; it exits when the service is removed. NT only: 9x cannot
  serve a named pipe. The pipe and security calls, CancelIo (not in 95) and
  the W window calls are looked up at run time, so the x86 program still
  loads on 95 and 98 -- run check-imports.py on the x86 build to be sure.
- *Windows 2000 features, by run-time lookup* (no build flags: one program
  per processor; 2000 shipped only for x86, though Alpha has betas):
  "Insert lock" (control key 101) calls LockWorkStation, through the tray
  when it runs (the call wants the user's desktop), else from the agent;
  "Block user input" (OptionMessage.block_input, field 5, a new core hook
  -- the last member of cdv_hooks, so the agents that fill it in order are
  unchanged) calls BlockInput from the main loop's thread; a session's end
  unblocks. NT 4.0 has neither (a log line says so); 98 has BlockInput.
  **None of it tried on Windows 2000 yet.**

Checked here before sending to the Alpha: every NT source parses as C89
(clang, `-Werror=declaration-after-statement`) and has no undeclared calls
(C99 pass) against MinGW-w64's headers, with windows.h first and MinGW's
stdint.h in place of the shim; MinGW's headers do not hide post-NT 4.0
calls, so those were checked by hand (only through GetProcAddress). The
core's test_core passes; selftest-host passes. stage.py's NT rules now
depend on core\*.h too (a changed core struct rebuilds the NT objects).

Next (the user's plan): the terminal, run by the tray as the logged-on
user (not SYSTEM), with a tray item to restart it; RustDesk's persistent
terminal sessions (Terminal.service_id, TerminalOpened.replay_terminal_
output) fit "always running, see earlier output". Then TCP tunneling
(LoginRequest.port_forward, field 8). Upstream's protocol for both:
rustdesk/rustdesk libs/base/protos/message.proto.

## 2. The build: Visual C++ 4 on the machine, sources staged here

The C89 compiler is the only one there is for NT on MIPS, Alpha and PowerPC,
and it runs on NT itself. `scripts/stage.py` makes `build/stage` (core,
libsodium-min, BearSSL, zstd, `src/`, `res/`) and its nmake `makefile`;
`scripts/ntx.py sync` sends what changed to the NT machine; `nmake` there.
`make nt` does all of it. A full build is ~3 minutes under QEMU (BearSSL's
294 files are most of it); after a change, seconds.

What the staging rewrites, and why (each in `stage.py`):

- `long long` / `ULL` / `LL`: MSVC 4 has `__int64` and `ui64`/`i64` (checked:
  64-bit multiply and shift right under `/O2` on PPC).
- C99 variadic macros (zstd only): fixed parameters kept, calls truncated --
  they only carry log text.
- `anchors.c`'s designated union initialisers: an EC key is written through
  the RSA member (same layout on ILP32; every NT is ILP32, the Alpha too).
- `__declspec(noinline)`: Visual C++ 7.

What headers cover: `src/compat.h` (forced into every file with `/FI`):
`inline`, `__attribute__`, `CRYPTO_ALIGN`, `near`/`far` (keywords to MSVC;
`vp8enc.c` has a variable called `near`), zstd's deprecation attribute,
`snprintf` (the IRIX port's, which terminates and counts -- MSVC's
`_snprintf` does neither). `shim/stdint.h`. zstd is compiled with
`/U_MSC_VER` so it takes its generic C path instead of intrinsics MSVC 4 lacks.
`src/win.h`: `<windows.h>` minus `MOD_SHIFT`/`MOD_CONTROL`, which clash with
`session.h`'s key modifiers (other values).

One change was made to the shared core for C89: a declaration after a
statement in `vp8enc.c` (`sixtap8x8`). `make -C ../crustdesk-macos-classic/host
test` passes after it (on this machine it needs `LIBVPX=/usr/lib/x86_64-linux-gnu/libvpx.so.12`,
and GCC 14+ wants `CC="cc -Wno-error=implicit-function-declaration"` for
`test_crypto.c`; neither is new).

## 3. Design

- **One thread does the screen, input and sessions** (`agent.c`), and it
  owns no window: `SetThreadDesktop` fails for a thread with windows, and the
  agent follows the input desktop (`screen_follow`, every 250 ms) --
  Default, Winlogon (logon, locked, Ctrl-Alt-Del), Screen-saver. Hence the
  **clipboard has a thread of its own** (`clip.c`: a viewer window; setting
  the clipboard needs an owner window), DNS lookups a thread, the console
  reporter a thread (it never touches `rng.c`, which is the main thread's).
- **Capture** (`screen.c`): `BitBlt` from the screen DC into a top-down
  32-bit DIB section (GDI converts from any depth), compared with the last
  look scanline by scanline (`differ`: a word loop; MSVC's PPC `memcmp` goes
  bytewise and is 4x slower), changed macroblocks converted to I420. A look
  reads the rows around the pointer, then bands of the rest until
  `LOOK_BUDGET_MS` (25): a whole 1280x1024 `BitBlt` is ~90 ms in QEMU. With
  nothing changing for 2 s it looks every 200 ms instead of every frame.
- **The frame** is the IRIX port's: a keyframe of the background colour, then
  the picture painted in within a budget, oldest-dirty and nearest-pointer
  first; a few macroblock rows encoded between looks at the network. No
  motion hints (NT gives no damage or scroll information without a mirror
  driver).
- **Cursor**: `GetCursor()` answers for the calling thread, so the agent
  joins the foreground thread's input for a moment (`AttachThreadInput`), as
  WinVNC did. Monochrome cursors' inverting pixels are drawn black.
- **Input**: `keybd_event`/`mouse_event` (`SendInput` is SP3+). Map mode:
  the client sends set-1 scancodes (`0xE0nn` extended); the virtual key comes
  from this machine's layout. Legacy/Unicode: `VkKeyScanW`, else Alt+0nnn.
  **Ctrl-Alt-Del**: a thread moves to the Winlogon desktop and posts
  `WM_HOTKEY` to "SAS window" (needs SYSTEM). Lock screen (101) cannot be
  done on NT 4.0 (no `LockWorkStation`); logged.
- **Service**: LocalSystem, `SERVICE_INTERACTIVE_PROCESS` (the interactive
  window station), auto-start, depends on Tcpip. The agent also calls
  `SetProcessWindowStation(WinSta0)`.
- **Settings** (`prefs.c`): `C-Desk-Vint.cfg` beside the program, key=value
  like the other ports, replaced atomically, DACL Administrators+SYSTEM (only
  on NTFS -- see section 6). The agent writes `C-Desk-Vint.sta` for the
  window (registered, refused, sessions). The window edits the file and
  restarts the service.
- **Windows 95/98** (`plat_nt()` is 0): no service manager -- `-install`
  writes HKLM `...\RunServices` (started at boot, before logon), the agent
  calls `RegisterServiceProcess` (lives through a logoff), holds the mutex
  "C-Desk-Vint agent" (running) and waits on the event "C-Desk-Vint stop".
  Files through the A calls in the code page (`x_` wrappers in fs_win32.c),
  clipboard as CF_TEXT, `VkKeyScanA`, `GetUserName`; no desktops to follow,
  no Ctrl-Alt-Del to send (logged), no ACLs. NT-only calls are imported all
  the same: 95 and 98 export them as failing stubs.
- **Identity**: generated on first run like the others (9-digit ID, uuid,
  Ed25519 seed); entropy from `QueryPerformanceCounter` jitter, the clocks,
  memory, the pointer, and `CryptGenRandom` when CryptoAPI is there.

## 4. Traps

- **NT 4.0's `copy` has no `/y`** (it never asks), and NT 4.0's `netstat -an`
  does not list listening TCP sockets.
- **Injected modifiers overtake keys** on a slow machine: shift-N-T-4 typed
  as "Nt4", a Legacy "k" came out "K" (the next character's shift), an
  Alt+0233 lost. `input.c` sleeps 15 ms around each modifier change and after
  each Alt-code digit; all three then came out right.
- **MSVC 4 headers lack** `CP_UTF8` (NT 4.0 has the code page: `cliptool.c`
  defines it), `ICON_BIG`/`ICON_SMALL`; `ES_NUMBER` is there.
- **Threads need `/MT`**: Visual C++ 4 links the single-threaded C runtime
  by default, and the clipboard, lookup, console and Ctrl-Alt-Del threads
  call malloc. Built with `/MT` and `_beginthreadex` (`plat_thread`) since
  2026-10-30; the first PowerPC release (20261030-7dba170c8) has the bug.
- **nmake rebuilds nothing when flags change**: every object depends on
  `FLAGS` (the compiler lines, written by stage.py), the build ID is a
  header.
- **libsodium includes `<intrin.h>` for any x86 MSVC**; it came with Visual
  C++ 2005. Staged with `_MSC_VER >= 1400` added.
- **ntexec served one request at a time**, so an abandoned build blocked
  everything; now a thread per connection, and a RUN whose client went away
  is killed. (The PowerPC machine still runs the old one; rebuild it there.)
- **NT 4.0 x86 under KVM** hung at a blank desktop at 100% CPU (QEMU
  logging slirp send failures): the x86 machine runs on TCG. A full build
  is ~15-20 minutes there.
- **NT 4.0 turns automatic logon off** when the password is empty: the x86
  machine's Administrator password is "password" too.
- **The Makefile's own variables** must not be named `NTX` (the environment
  variable ntx.py reads): `NTX=host:port make nt` overrode the command.
- **The test image's password is "password"**, not blank as its README says
  (Winlogon's `DefaultPassword`); it logs on by itself.
- **`kill NAME` in the Resource Kit** matches window titles too; use PIDs
  from `tlist` when two copies run (the console agent, the service and the
  window are all C-Desk-Vint.exe).
- **The modern ID server wants a key exchange on TCP** (RendezvousMessage
  field 25, the client's `secure_tcp`): `rdvpoke.py` does it. The agent only
  sends hbbs single TCP messages (RelayResponse, LocalAddr) in the clear,
  which rs-ny accepted.
- QEMU user networking shares the host's public address, yet rs-ny chose a
  punch-hole/relay rather than a local-address session for `rdvpoke.py`.

## 5. The test machine

`scripts/qemu-40p.sh` (headless; VNC 127.0.0.1:5941, QMP socket) on
`/run/media/dani/BigDisk/winnt-ppc` (`NTVM`): `dl/nt40powerpc-qemu.tar.lz`
(tenox7, 156 MB, downloaded 2026-10-29 from
`tenox.pdp-11.net/os/winnt/VMs/qemu-ppc/`), unpacked in `img/` with its own
QEMU 11.1.1 (source: `github.com/tenox7/qemu`, branch powerpc; not kept --
the image's binaries are all that is needed), the image's disk read-only, our changes in the overlay
`work.qcow2` (+ `work-nvram.bin`). 128 MB, S3 Trio64 at 1280x1024, NE2000,
NT 4.0 SP2, Visual C++ 4 (C:\MSDEV), the Platform SDK (C:\MSTOOLS), the
Resource Kit (`kill`, `tlist`, `reg`, Perl).

On the overlay: `C:\ntexec` (ntexec, cliptool), ntexec in All Users'
Startup (it runs at the automatic logon), the build tree `C:\cdv\src`, and
the agent installed as a service in `C:\Program Files\C-Desk-Vint` with ID
782 582 429 (build 20261030-7dba170c8, since 2026-10-30 a /MT dev build). Its
settings file now points at **the user's own ID server, key, relay and
console** (personal settings, in that file only, as on the Indigo). NT was shut
down cleanly at the end of the first session (the Resource Kit's `shutdown
/L /Y /C /T:0`, then QMP `quit`). Throw `work.qcow2` away to start over.

`scripts/ntvm.py` drives the screen and keyboard over QMP (screenshots;
`sendkey`; a CD for getting ntexec.c in -- that is how it got there the first
time, typed at a command prompt). After that, `scripts/ntx.py`.

The x86 machine: `scripts/qemu-nt4x86.sh` on
`/run/media/dani/BigDisk/winnt-x86` (VNC 127.0.0.1:5942; ntexec on 2371, the
agent on 21128): tenox's `nt4-vanilla.rar` (unpacked with Ubuntu's
`unrar-nonfree`, fetched by `apt-get download` into `tools/`, since Ubuntu's
7-Zip has no RAR), image read-only, overlay `work.qcow2`; Visual C++ 4.0 for
x86 (Disc 1, from the user's NAS: `software/winworld-pc/extracted/visual-c/
4x`) copied to C:\MSDEV, in the system environment; automatic logon;
ntexec in Startup; the agent installed as a service (ID 284 688 195, generic
settings). `/run/media/dani/BigDisk/win9x`: 98 SE's ISO and 95 RTM's and 98
SE's system DLLs (`dll95/`, `dll98/`) for check-imports.py.

The Windows 98 machine: `scripts/qemu-win98.sh` on
`/run/media/dani/BigDisk/win98vm` (VNC 127.0.0.1:5943; ntexec on 2372, the
agent on 21138): the user's `Windows 98.vmdk`, read-only, run from a raw
copy `work.raw` (mtools can reach its files with the VM stopped: the
partition is at byte 1048576). MSDOS.SYS there now shows the start-up menu
and logs the boot. Its own restart stalls under QEMU: shut it down
(`rundll32 shell32.dll,SHExitWindowsEx 1`) and start QEMU afresh. The new
hardware wizards (QEMU's 440FX chipset, IDE, VGA) are cancelled every boot:
installing their drivers made it hang at the splash screen; so the CD drive
is not set up -- files go in on a floppy (`change floppy0 IMG raw` over QMP).
ntexec and cliptool are in C:\NTEXEC, ntexec in StartUp; the agent is
installed (ID 491 362 745, generic settings). Logon: user win98user, blank.

The user's NAS (`smb://daninas.local/software`, mounted by GVFS) has the
winworld collection: Visual C++ 4.0 RISC for MIPS and Alpha (4.0a), NT 4.0
install ISOs, 95/98 media; `inventory.tsv` lists it, `extracted/` has ISOs.
Tenox has an NT 4.0 MIPS QEMU image with Visual C++ 4.1
(`winnt/VMs/qemu-mips/nt4mips-thisone-vc41.rar`).

## 6. What is left

1. **Windows 95** on a machine.
2. **Other processors.** x86, ARM and Alpha are done (above). MIPS:
   QEMU's `magnum` runs NT 4.0 MIPS; needs an install and Visual C++ for
   MIPS. Run `-selftest` there, and on the Alpha (not yet run).
3. **The settings file on FAT** keeps no permissions, and the test machine's
   C: is FAT: the password and private key are readable by anyone who logs
   on. Moving those two into an ACL'd registry key would fix it on any disk;
   not done (the user's call).
4. **Speed**: 1.3-3 fps under QEMU while things change, most of it reading
   and comparing the screen. On the Alpha (2026-11-05, 800x600, the
   user's log, 10 s lines averaged): idle, read 4.5 s + convert 1.4 s of
   every 10 (0.1 fps); busy, read 4.6 + convert 1.6 + encode 1.7 (2.5 fps);
   a whole-screen read 1.1-2.2 s. Reading through GDI is the cost, which no
   compiler flag touches. So: with nothing changing for 5 s the screen is
   looked at once a second, not five times (QUIET_*; the peer's input
   brings it back at once), and the first-look line logs the depth.
   **Written, not yet built** (2026-11-05): the screen hooks, WinVNC's
   way. cdvhook.dll (hook/cdvhook.c; WH_GETMESSAGE: WM_PAINT's update
   rect, keys and clicks mark their window; WH_CALLWNDPROC: frames, moves,
   scrolls, owner-drawn items, menus) marks 16-pixel squares in a named
   shared block (hook.h: InterlockedExchange both sides, no DACL -- the
   worst a program can do is cost a read). The agent sets the hooks from a
   thread on Default for each desktop session only (hooks.c; gone between
   sessions, and when the agent dies); a look then reads the marked
   squares, +-64 px round the pointer, every ConsoleWindowClass (csrss
   draws those, unhooked) and 2 rows of a round, as merged rectangles
   (screen.c collect_hinted); Winlogon is read the old way. Setting hook=1
   by default; the settings window's checkbox, -hook on|off. The nmake
   build makes the DLL first and carries it inside the .exe (RCDATA,
   CDV_HOOK_DLL), written beside it when the agent starts; PINT's build
   makes it as a DYNLINK component (make-dll.sh, DLLBASE 0x68000000)
   shipped beside, non-fatal if it fails. Checked: C89 and undeclared
   calls, as above. Untried: everything at run time, on every processor.
   Then (same day): the hooks worked on both machines (reads 10x cheaper
   on the PowerPC, idle reads 0.2 s per 10 on the Alpha, from 4.5), but
   keys and clicks marked whole windows and consoles were read in full
   every look; now a key marks a band round the caret (all below it for
   Enter/Backspace/Delete), consoles are read 300 ms (front) / 2 s (the
   rest) apart, a look reads at most SPAN_ROWS rows per rectangle and keeps
   what it had no time for. Profiled (VP8E_PROFILE, vp8e_clock from the
   performance counter): on the Alpha encoding is then the largest cost,
   ~2.6 ms a changed macroblock, spread over its phases (transform 32%,
   mode search 26%, tokens 25%, reconstruct 13%). **Double-clicks broke**
   (the console too, so not the DLL): the encoder held the loop for 4
   macroblock rows (~0.5 s on the Alpha) between a click and the next;
   now it codes a row at a time for at most SLICE_MS between looks at the
   network, and no new look or frame starts for CLICK_HOLD_MS (300) after
   the peer's button. Untried yet. The log folder is a setting (logdir,
   the window, -logdir).
   Updates came in as blocks (the user's word): a frame coded only 150 ms
   of macroblocks, from whatever had been read so far. Now FRAME_BUDGET is
   600 ms (coding is sliced, so input is not held up) and a frame waits up
   to GATHER_MS (500) for the look's leftovers. Untried.
   **Double-clicks, the real cause:** `current_cursor()` called
   AttachThreadInput every 100 ms in a session, to ask GetCursor for the
   foreground thread's pointer. Windows keeps double-click state in the
   input queue, and that appears to have wiped it: for the machine's own
   mouse too (0 of 3 through the KVM during an idle session, against 12 of
   12 with none) and all 45 scripted tries. Since the first NT release;
   nobody had tried a double-click. Now GetCursorInfo where there is one
   (SP3, 98 and later); else attach only when the pointer moved, so an
   hourglass with no movement shows when the pointer next moves. The user
   reports double-clicks working on the PowerPC (SP2, the fallback); the
   Alpha (SP6, GetCursorInfo) is not built yet. Ruled out on the way, by
   test: the hook DLL, the move before each click, timing, the SYSTEM
   account (going through the tray failed the same). With none of that,
   the window is about 0.4 s through the KVM; the setting is 500 ms.
   Also from that: a desktop session widens the double-click time to 900
   ms (widen_dblclk, restored at the end), never shown to be needed; the
   screen work runs below normal priority; a new look waits for twice what
   the last look and frame took; the log gives cpu % and every click (both
   for now); a console in front is read 300 ms apart only for 3 s after the
   peer's input, else every 2 s (it was 60% of the machine in a session).
   With no session the agent is at 0% cpu.
   Stale areas while things changed: the hooks mark a square when a program
   is told to draw, before it has, so a fast read saw the old picture and
   nothing looked again until the round. Every marked square is now read a
   second time ECHO_MS (300) after the marks stop. Diagnosis from the code;
   the user's verdict after it: still slowish, maybe as good as it gets.
   **The PowerPC's network dies** after an hour or two up (ping 3-5 s, then
   nothing) with the CPU idle and the agent at 0%: the user knows it as a
   core bug. Shut NT down, reboot the MiSTer, load the core again.
   Damage without reading, as XTrap gives IRIX: NT 4.0 has no documented
   mirror drivers (those are Windows 2000's), though NetMeeting's mnmdd
   did this on NT 4.0 SP3 and later, needing a reboot, and not on our SP0
   baseline; the user-mode way is WinVNC's, a
   hook DLL in every GUI process (WM_PAINT, window moves) giving rectangles
   to read, with polling for what it misses (consoles, carets) -- no
   kernel code, so every NT processor. A display driver of our own would
   be per card and per processor.
5. Chat and the tray: working on the Alpha (above); not on 9x. Lock and
   block input: Windows 2000 only, not tried.
6. Released (the user asked): build **20261030-feaa6be71**, x86 and PowerPC
   (`C-Desk-Vint-WinNT4-{x86,PPC}-20261030-feaa6be71.{exe,zip,SHA256SUMS,
   MANIFEST.txt}`) in `r-deskway-1.5.0`, replacing 20261030-0df59d3a8 (whose
   assets were deleted; it differed only in the settings window), with rows
   in both of the notes' tables and a "C-Desk-Vint for Windows NT 4.0 (x86
   and PowerPC) and Windows 98" section (notes CRLF); copies in
   `~/vintage-state/crustdesk-winnt/`. Each was installed on its machines as
   uploaded (bytes compared) and tested there; the MANIFESTs list exactly
   what, and which tests ran on the build before.
7. The user's Windows 98 VM is backed up on the NAS as
   `software/VM Images/Win98VM-20261030.zip` (the VMware files as they were,
   the QEMU working disk with the agent and tools, a README).
