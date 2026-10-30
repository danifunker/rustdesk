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

**Windows 95 and 98** (the x86 build only; same program): it *loads* there
-- `scripts/check-imports.py` finds every import in 95 RTM's and 98 SE's
DLLs (from the user's NAS) -- and the 9x branches are written (section 3),
but **nothing has run on 9x yet**: no 9x machine (98 SE setup wants a
product key; asked the user).

**Not proven**: MIPS and Alpha builds (not compiled: no NT machine or
compiler for them yet; nothing in the source is processor-specific, and
`-selftest` exists to check them); real PowerPC hardware; NT 4.0 with other
service packs; a real RustDesk console; a screen depth other than the 40P's
(GDI converts any depth into the 32-bit DIB, so it should not matter);
`restart` from a peer; chat (not implemented: a service has nowhere to show
it).

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

The user's NAS (`smb://daninas.local/software`, mounted by GVFS) has the
winworld collection: Visual C++ 4.0 RISC for MIPS and Alpha (4.0a), NT 4.0
install ISOs, 95/98 media; `inventory.tsv` lists it, `extracted/` has ISOs.
Tenox has an NT 4.0 MIPS QEMU image with Visual C++ 4.1
(`winnt/VMs/qemu-mips/nt4mips-thisone-vc41.rar`).

## 6. What is left

1. **Windows 95/98 on a machine**: install one (98 SE setup needs a
   product key from the user) and run the 9x branches; then the release
   notes may say so.
2. **Other processors.** x86 is done (above). MIPS:
   QEMU's `magnum` runs NT 4.0 MIPS; needs an install and Visual C++ for
   MIPS. Alpha: needs real hardware or an emulator that runs NT. Run
   `-selftest` on each.
3. **The settings file on FAT** keeps no permissions, and the test machine's
   C: is FAT: the password and private key are readable by anyone who logs
   on. Moving those two into an ACL'd registry key would fix it on any disk;
   not done (the user's call).
4. **Speed**: 1.3-3 fps under QEMU while things change, most of it reading
   and comparing the screen; real hardware unmeasured. A mirror driver would
   give damage regions and motion; NT 4.0 had them (VNC's), but that is a
   kernel driver per processor.
5. Chat, and the peer's "lock screen", are not offered.
6. Release: build 20261030-7dba170c8 from `make dist`
   (`C-Desk-Vint-WinNT4-PPC-20261030-7dba170c8.zip`, `.exe`, `.SHA256SUMS`),
   copied to `~/vintage-state/crustdesk-winnt/`; not uploaded anywhere yet.
