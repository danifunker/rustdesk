# C-Desk-Vint for Windows NT

A RustDesk agent for Windows NT 4.0 on every processor NT 4.0 ran on --
x86, MIPS, Alpha and PowerPC -- from one C source. It is the classic Mac
agent's portable core (`../crustdesk-macos-classic/src/core`: the protocol,
the encryption, the ID server and relay, the VP8 encoder, file transfer,
PNG, the console's https) with an NT side here, the IRIX port's shape
(`../crustdesk-irix`): one `select()` loop over the ID server, the relay and
the peer, the screen read back between them.

`pkg/README.txt` is the user's read-me (it ships in the package);
`RESUME.md` is where it stands, what is proven and the traps.

## Layout

| | |
|---|---|
| `src/agent.c` | the main loop: sessions, frames, the ID server, LAN discovery |
| `src/screen.c` | GDI capture into a 32-bit DIB section, compared and converted to I420 a band at a time; following the input desktop; the cursor's shape |
| `src/input.c` | `keybd_event`/`mouse_event`; Windows scancodes in Map mode; Ctrl-Alt-Del through Winlogon's SAS window |
| `src/clip.c` | the clipboard, in a thread with a viewer window of its own |
| `src/fs_win32.c` | `cdv_fs_ops` over the W calls: drives at "/", `C:\paths` |
| `src/report.c` | the console reporter, a thread |
| `src/service.c` | the service: install, start, stop, `ServiceMain` |
| `src/ui.c`, `res/` | the settings window, the icon, the version |
| `src/prefs.c` | `C-Desk-Vint.cfg` and `.sta`, shared by agent and window |
| `src/selftest.c` | `-selftest`: the core's results against GCC's and libsodium's |
| `src/compat.*`, `shim/stdint.h`, `src/win.h` | what Visual C++ 4 lacks |
| `scripts/stage.py` | the tree the NT machine compiles, and its makefile |
| `scripts/ntx.py`, `tools/nt/ntexec.c` | driving a build machine over TCP |
| `tools/nt/cliptool.c` | the clipboard from a command line, for tests |
| `tools/make-icon.py` | `res/cdv.ico`, in the icon format NT 4.0 reads |

## Building

The compiler is the Visual C++ that ran on NT for each processor (4.x on
the PowerPC test machine: `cl` 10.10; 6.0 built the Alpha release unchanged;
2.x and 5.0 have the same C front end). It knows C89 and nothing of C99, so the sources are
*staged*: `scripts/stage.py` copies the core, libsodium-min, BearSSL, the
zstd decoder and `src/` into `build/stage`, rewrites what a header cannot
cover (`long long` to `__int64`, `ULL` suffixes, C99 variadic macros in zstd,
designated initialisers in `anchors.c`), and writes an nmake makefile. The
staged tree has nothing processor-specific: `nmake` in it on any NT machine
with Visual C++ builds `C-Desk-Vint.exe` for that machine's processor.

With an NT machine running `tools/nt/ntexec.c` (build it there with
`cl ntexec.c wsock32.lib`; it has no authentication, so only behind a
loopback-only forward):

    make nt                  # stage, send what changed, nmake there, fetch the .exe
    make selftest-nt         # run -selftest there
    make install-nt          # and replace the installed service's program
    make selftest-host       # the same selftest built by GCC here
    make dist                # the release files for ARCH (default ppc)

`NTX=host:port` says where ntexec listens (default `127.0.0.1:2370`, the
QEMU forward), `ARCH` names the output (`ppc`, `mips`, `alpha`, `x86`).

By hand: copy `build/stage` to the NT machine, `nmake` in it.

**ARM (PINT, NT 4.0 on the Raspberry Pi 2/3/4, github.com/tenox7/PINT)** is
the exception: no compiler ever ran on NT for ARM, so PINT's own build makes
it, in Docker (`tenox7/msvc-arm32`, Microsoft C for ARM), as one of PINT's
apps, linked with NT 4.0's C run-time and import libraries built from the
NT 4.0 tree. `scripts/stage-pint.py` flattens `build/stage` into that app
(`build/pint/cdv`: its SOURCES components), `scripts/pint-build.sh` builds
only the parts of PINT the agent links and then the agent (`make pint
PINTWORK=dir`; its header says how the trees are laid out). The workflow
`.github/workflows/winnt-pint-agent-build.yaml` does all of it on a GitHub
runner, fetching both trees at pinned commits. It also builds `ntexec.exe`
for ARM, so `ntx.py` can drive a PINT machine for `-selftest`.

## Testing

`make -C ../crustdesk-macos-classic/host test` tests the shared core;
`-selftest` checks this compiler built it the same (it is in the release
build, so a machine of any processor can be checked). A session is driven
from Linux with `../crustdesk-macos-classic/host/cdvpoke.py` (by address)
and `rdvpoke.py` (by ID, through an ID server and relay, as a client on the
internet comes); `save FILE` then `host/build/vp8dump` shows what the peer
saw. In Map mode a key is a Windows scancode (`key 30` is A; `0xE048`,
57416, is Up); `ctrl 100` is Ctrl-Alt-Del.

The PowerPC test machine is QEMU's IBM 40P with NT 4.0 and Visual C++ 4
(tenox7's image, `virtuallyfun.com`, 2026-10-19); see `RESUME.md`.
