# RESUME — RustDesk agent for IRIX (SGI MIPS)

Pick-up point. Last updated 2026-08-19, end of the session that **got the agent
itself running on IRIX**: a peer connects over the real protocol, logs in, and
receives VP8 video, and mouse injection works.

The previous session's blocker was not real. Cross-compiled X11 clients were
never at fault: the X server on the image had wedged, and *every* client hung
against it, IRIX's own included. A freshly started server answers our binaries
immediately. See §THE BLOCKER, RESOLVED.

Capture is built and verified on hardware, in C and in Rust. Rust std builds for
`mips-sgi-irix6.5`, the whole agent compiles and links, and `rustdesk-agent`
serves a real session.

**The one thing standing between this and a usable remote desktop is not in the
agent.** The emulated X server wedges after roughly one full-screen frame, so
throughput cannot be demonstrated under IRIS. Everything above the X server
works; see §THE AGENT RUNS and §THE REMAINING BLOCKER.

---

## Current state in one screen

```
goal            port rustdesk-ppc-agent (../rustdesk/rustdesk-ppc-agent) to IRIX
target hw       O2 primary; must also run on Octane, Fuel, Tezro. NOT YET BUILT.
                Develop against the emulated Indy; O2 exists only later.
ABI             n32 first (mogrix). o32 (rust-irixlibstd) is the later compat build.
5.3             deferred, stretch goal, C acceptable. Do not start it.

toolchain       /opt/cross/bin           clang-18 + ld.lld-irix symlinks
                /opt/irix-sysroot        pulled from the 6.5.22m image
                /opt/sgug-staging/usr/sgug   staging; irix-cc lives in bin/
compiler        /opt/sgug-staging/usr/sgug/bin/irix-cc

rust            ports/rust/rustup   PRIVATE RUSTUP_HOME (nightly 1.99.0, IRIX-patched)
                ports/rust/cargo    PRIVATE CARGO_HOME  (IRIX-patched crate registry)
                source ports/rust/env.sh before any cargo command

emulator        ports/iris-run/iris-target/release/iris   PRIVATE build, has chd
                (~/repos/iris/target/release/iris no longer does — see Boot)
disk            ~/Indy-IRIX65_dev.chd            IRIX 6.5.22m, R5000, /usr/sgug populated
backup          ~/Indy-IRIX65_dev.chd.bak-before-first-write   pristine, keep
run config      ports/iris-run/iris.toml         ci_socket is now /tmp/iris-rdagent.sock
nvram           ports/iris-run/nvram-irix65.bin  has console=d; do not lose it
shells          ports/iris-run/irixsh.py         telnet driver (superseded, see gsh.py)
                ports/iris-run/gsh.py            marker-based telnet runner — use this
telnet/ssh      host 2324 -> guest 23,  host 2222 -> guest 22
guest net       192.168.0.2, gateway/host 192.168.0.1

mogrix fork     ~/repos/mogrix, branch danifunker-ports (remote danifunker-origin)
```

**Do not modify ~/repos/iris.** Dani is handling iris changes in a separate
session. Note what you wish were different; do not change it.

**Another Claude session shares this machine** (`~/repos/rust-irixlibstd`, the
o32 effort). It runs its own iris on the default `/tmp/iris.sock` and uses the
shared `~/.rustup` and `~/.cargo`. Two things follow, both already handled and
both easy to undo by accident:

- `iris.toml` sets `ci_socket = "/tmp/iris-rdagent.sock"`. iris *deletes and
  rebinds* whatever socket path it is given, so running on the default would
  silently steal that session's control socket.
- Our Rust work uses a private `RUSTUP_HOME`/`CARGO_HOME`. Both mogrix steps are
  destructive to shared state — `patch-rust-sysroot.sh` rewrites nightly's std
  in place and `mogrix patch-crates` rewrites crate sources in the registry —
  and neither is namespaced by target.

---

## What is done and verified on hardware

Every one of these was run on IRIX 6.5.22m / R5000 under IRIS, not just built.

| Component | Evidence |
|---|---|
| **libsodium 1.0.18** | Ed25519 combined-form sign/verify, X25519 `crypto_box`, XSalsa20-Poly1305 secretbox — all pass. Zero source patches. |
| **libvpx 1.13.1** | VP8 encode→decode round trip at 1280x1024, 640x512, 320x256. Y mean abs error 0.4–2.0. One patch. |
| **mbedTLS 3.6.2** | Its own selftest: 30 suites, all pass. Two patches. |
| **zstd 1.5.6** | Built for n32 with one `make libzstd.a CFLAGS="-O2 -DZSTD_MULTITHREAD=0"`, no patches. Installed to the staging lib32; `clipboard.rs` links it. |
| **The PPC TLS shim** | `tls_shim.c` compiles unmodified for IRIX and links against our mbedTLS. Now also **linked into a running Rust binary**. |
| **X11 clients** | Cross-built `probes/minimal` opens the display and reports 1280x1024 depth 8. The toolchain was never the problem. |
| **Capture, C** | `src/capture_shim.c` + `probes/capture_test.c`: all three paths work, rectangles verified against canvas pixels. |
| **Capture, Rust** | `src/capture.rs` + `capture-selftest`: damage path, band mapping, downscale, and rebuild-after-drop all pass. |
| **Rust std for n32** | `-Zbuild-std` builds std for `mips-sgi-irix6.5`; `hello-irix` runs (HashMap, format!, env, fs, current_exe). |
| **Agent portable modules** | json, convert, zstd_frame, frame, http, png, crypto, config, sys, encode + both protobuf modules compile **and run**: 20/20 checks pass on the Indy, including a real TCP GET and libsodium round trips. |
| **The whole chain** | `pipeline` runs capture -> downscale -> I420 -> VP8 encode on the Indy and reports per-stage timings. See below. |
| **The agent** | `rustdesk-agent` builds from the PPC `main.rs` and serves a session: `testpeer` logs in over the real protocol and receives a VP8 keyframe. |
| **Input injection** | XTEST, via `src/input_shim.c`. Absolute moves, relative moves and display clamping all verified by `--probe-live`. |
| **Rust process spawning** | `std::process::Command` spawn/kill/wait works — verified by having the self-test start `xclock` to generate damage. |

---

## THE AGENT RUNS

`ports/rust/agent-portable` builds `rustdesk-agent` from the PowerPC tree's own
`main.rs`, and it works on the Indy:

```
agent id  : ff6izj02b
display   : 1280x1024
[  0.33] INFO  lan discovery listening on udp/21119
[  0.33] INFO  agent listening on 127.0.0.1:21118 (id ff6izj02b)
```

`src/bin/testpeer.rs` is a client that speaks the real protocol — the same
handshake `session.rs`'s own tests use, which is the same one upstream's client
uses. Run on the guest against the agent over loopback:

```
LOGGED IN: 1 displays, hostname 'IRIS', platform 'Linux'
  display 'Display' 1280x1024 at 0,0, online true
  video frames   1 (1 key), 141320 bytes total
  first frame    23425 ms after login
VERDICT: the agent is serving video to a peer.
```

So: TCP, framing, the direct-IP handshake, the password hash, `PeerInfo`, the
video pump, VP8 encoding and delivery all work. **Mouse injection works too**,
verified by the agent's own `--probe-live`:

```
  cursor before : 640,512      asked for: 426,341   after: 426,341  => WORKS
  relative      : +60,-40      after: 486,301                       => WORKS
  +5000,+5000   : cursor at 1278,1022    => the bound holds
```

(`--probe-live` then reports "mouse click changed nothing" and "key injection
changed nothing". That is the probe expecting a macOS Apple menu to open; there
is no such thing here, and no window manager running on the bare server. It is
not an input failure — the framebuffer probe in the same run shows bands going
dirty when the click lands.)

### What it took

| Problem | Fix |
|---|---|
| `session.rs`/`main.rs` gated the whole video pump to macOS | Widened every gate to `any(macos, irix)`. Purely additive: on macOS `any(macos, irix)` is exactly `macos`, so the Mac build cannot be affected. |
| `clipboard.rs` links libzstd, which IRIX has not got | Cross-built zstd 1.5.6 (`make libzstd.a`, one command, no patches) into the staging lib32. |
| No input injection | `src/input_shim.c`: the same C interface `input.rs` already calls, over XTEST. All the decisions stay in `decide_mouse`/`decide_key`, which are pure and already tested. |
| Mac virtual keycodes mean nothing to X | The shim maps them to X **keysyms** and resolves keycodes at runtime, so the person's actual layout is respected. A character the layout cannot produce is typed by borrowing a spare keycode, xdotool-style. |
| `set_read_timeout` fails with ENOPROTOOPT (99) | IRIX has no `SO_RCVTIMEO`. It was fatal — the peer logged in, the encoder started, and the session was dropped a second later. The loop now paces itself with `poll(2)` via `sys::wait_readable`. |
| The agent reported itself as "Mac OS" | Now "Linux" on IRIX. Clients key real behaviour off this, notably keyboard translation; "Linux" rather than "IRIX" because clients match against a known set and IRIX is an X11 desktop with Control-based shortcuts. |
| `Capturer` lacked `refresh`, `read_band`, `invalidate_band` | Added, with IRIX semantics: the canvas is already current on the damage path, so `read_band` only does work for a band the caller forced. |

### LLD cannot link SGI's static archives

Worth knowing before reaching for any other SGI-only library. IRIX ships XTEST
as `libXtst.a` and nothing else, and LLD refuses it twice over:

```
ld.lld-irix: error: libXtst.a(XTest.o): invalid sh_info in symbol table
ld.lld-irix: error: ...(.text.foo): multiple relocation sections to one
                    section are not supported
```

The first is a genuine defect in SGI's objects — `sh_info` on `.symtab` is 0,
where the spec says it is the index of the first non-local symbol —
and `tools/fix-sgi-archive.py` repairs it. The second is a real gap in LLD's
MIPS support and cannot be papered over.

So `input_shim.c` issues the XTEST protocol requests itself. `XTestFakeInput` is
one request with a fixed 36-byte body; `Xlibint.h` is in the sysroot, the major
opcode comes from `XQueryExtension`, and the dependency disappears. Every X
library that ships *shared* links fine, so this only bites on the extensions SGI
shipped static.

---

## THE REMAINING BLOCKER: the emulated X server wedges under capture load

Reproducible, and it is the reason the frame count above is 1 rather than
hundreds:

- Peer logs in, the agent captures and encodes, one keyframe is delivered at
  ~23 s.
- The X server then stops answering. `Xsgi` stays in `ps` with its CPU time
  frozen; `xdpyinfo` hangs; the agent's `XOpenDisplay` retries fail, so it logs
  `rd_capture_open: XOpenDisplay($DISPLAY) failed` every retry interval for the
  rest of the session.
- `/root/restart-x.sh` recovers it every time, and the next session behaves
  identically: exactly one frame, then the same wedge.

This is the same fault as §THE BLOCKER, RESOLVED, now provoked by our own
traffic rather than by xdm. Things established about it:

- **Not the screen saver.** Armed at 45 s, the screen blanked and the server
  stayed healthy.
- **Not one specific call.** A single whole-screen `XShmReadDisplayRects` used
  to wedge it reliably; making `rd_capture_full` read in 128-row strips fixed
  `--probe-display`, which now runs a complete pass at 1280x1024. The session
  still wedges it, so the trigger is volume rather than any one request.
- **Not a double read.** The `Capturer` was re-reading all 64 bands right after
  a full read — a real bug, now fixed — but fixing it changed nothing here.
- The guest kernel sometimes prints `WARNING: ng1 pixel dma read timeout`
  alongside, and sometimes nothing at all.

**For the iris side:** sustained ReadDisplay traffic against the Newport
emulation stops the server making syscalls. `probes/capture_test.c` and the
`pipeline` binary both reproduce the healthy path; the agent under a live peer
reproduces the wedge within one frame.

**For the agent:** less capture volume is worth having regardless, and is the
next piece of work. See §Next steps.

### The agent can crash when the server dies underneath it

`/tmp/core` on the guest was an `IRIX N32 core dump of 'rustdesk-agent'`, from a
`--probe-display` run that was blocked against a wedged server when
`restart-x.sh` killed that server out from under it. A core means a fault, not a
clean exit, and this is exactly the situation the agent will meet in the field —
it is *the* recovery path this platform needs.

`capture_shim.c` installs an `XIOErrorHandler` that longjmps to whichever guard
is armed, because Xlib's default handler calls `exit()`; something is getting
past it. Worth reproducing deliberately (start a capture, kill `Xsgi`, see what
happens) and fixing before anything else is trusted to survive a restart. Not
investigated here — there is no `dbx` on the image, so a backtrace needs one
installed or a build with symbols and a hand-decoded stack.

---

## THE BLOCKER, RESOLVED — it was the X server, not us

The previous session concluded that cross-compiled X11 clients could not
complete the X connection setup while IRIX's own could. That conclusion was
wrong, and the way it was wrong is worth keeping.

**What was actually happening.** The X server xdm had started was wedged. It sat
in `ps` with its CPU time frozen, made **zero syscalls over 60 s of `par`**, and
`netstat` showed 12 bytes sitting unread in its listening socket's receive
queue — our setup request, never collected. The kernel completes a TCP handshake
from the listen backlog whether or not the server ever calls `accept`, so
`connect()` succeeded and then nothing happened, and 60 s later the server's own
authorisation timeout dropped the connection. That produced the EOF that looked
like a refusal.

**How it was settled.** `probes/rawx.c` speaks the 12-byte X11 connection setup
by hand over a plain socket, with no Xlib at all, and it can be compiled by
nekoware gcc as easily as by our toolchain. Against the wedged server it got no
answer and no EOF. That one result exonerated the toolchain, Xlib, the SGI shm
transport, `-Bsymbolic`, `safe_mem.o`, dlmalloc, and access control in a single
step — an X server that refuses a client still *answers*.

Killing the server and starting a bare `Xsgi :0 -bs -c` by hand made everything
work at once: `rawx` got `success=1` over TCP **from a telnet session**, IRIX's
`xdpyinfo` printed its report, and our cross-built `minimal` opened the display.

**So these are now closed:**

- Access control is not an issue. Telnet sessions connect fine. `/etc/X0.hosts`
  turned out to be irrelevant either way.
- The toolchain is not an issue. Native gcc failed identically against the
  wedged server.
- `-Bsymbolic`, `safe_mem`, dlmalloc and `R_MIPS_REL32` are all unrelated. Do not
  spend time on them.

**What wedges the server is a graphics-driver hang, and it is an iris matter.**
The guest kernel prints, on the serial console:

```
WARNING: ng1 pixel dma read timeout
WARNING: ng1: pixel dma timeout!
```

`ng1` is the Newport driver. A pixel DMA that never completes is exactly
consistent with a process asleep in the kernel making no syscalls. Ruled out as
the trigger: the screen saver — armed with `xset s 45 45 s blank`, the screen
blanked and the server stayed healthy. The wedged server was the one xdm starts
(`-bs -nobitscale -c -pseudomap 4sight -solidroot sgilightblue`, plus the visual
login rendering); a bare `Xsgi :0 -bs -c` with nothing drawing has stayed up for
hours. **This is worth reporting to the iris side as a Newport/REX3 DMA
completion bug.**

**Our own capture load wedges it too.** Later in the session, a `pipeline` run
doing repeated *full-screen* 1280x1024 `XShmReadDisplayRects` reads wedged the
server the same way — client blocked, `Xsgi` still in `ps`, CPU time frozen,
machine load 0.17 — and that time the console printed no `ng1` warning at all,
so the kernel messages are an occasional symptom rather than the whole story.
Short runs and small rectangles have never triggered it; hours of damage-path
polling have not either. **Treat full-resolution reads as the thing to avoid**,
which is also what the agent should do anyway.

That gives the iris side a reproducer: repeated whole-screen ReadDisplay reads
at 1280x1024, and the server stops answering.

**Practical handling:** `/root/restart-x.sh` is on the image and tested. It kills
Xsgi and xdm by pid, starts a bare server, turns the saver off, and prints
`xdpyinfo`'s first lines. If X looks unresponsive, run it before suspecting
anything else. Recovery is reliable: it has been used four times, and capture
works immediately afterwards including a rebuilt `Capturer`.

---

## Capture: what the server gives us, measured

`probes/xcapture.c`, `probes/xshmcap.c` and `probes/sgicap.c` established all of
this on the live server. It is a much better hand than the Mac port was dealt.

### The extension that changes the design: SGI-SCREEN-CAPTURE

The server has no DAMAGE extension — it predates it by a decade — but it
advertises `SGI-SCREEN-CAPTURE`, which is the same idea built for exactly this
job. `X11/extensions/sgicap.h`, implemented in **libXext**:

```c
SGICapRegisterInterest(dpy, drawable, x, y, w, h)   -> handle
SGICapStart(dpy, handle)
SGICapQueryAndReset(dpy, handle, &time, &count, &ordering)          -> rects
SGICapQueryCopyAndReset(dpy, handle, &t, &n, &ord, shmbuf)          -> rects AND pixels
SGICapStop / SGICapWithdrawInterest
```

`SGICapQueryCopyAndReset` returns the damaged rectangles **and copies their
pixels into a shared-memory buffer in one round trip**, then resets the damage.
Measured: 4–8 ms for a small update, rectangles exact and in screen coordinates,
ordering `YXBanded`. There is no software diffing, no sampled-row hashing, and
no full-screen poll in the steady state.

So the frame loop is one call. The PowerPC agent's whole `dirty_bands`
machinery — sampling every eighth row, hashing, hoping — is replaced by asking.

### Formats and costs (emulated R5000, 1280x1024; real hardware ~3x faster)

| Fact | Value |
|---|---|
| Screen | 1280x1024, **depth 8 pseudocolour** |
| ReadDisplay output | **32 bpp regardless of screen depth** — no palette on the capture path |
| **Memory byte order** | **A,B,G,R** — 0xff first, red last. Not the Mac's A,R,G,B, not libyuv's "ARGB" |
| `XRD_READ_POINTER` | **honoured** (`hints_ret=0x4`) — cursor composited, set `cursor_embedded`, never send a shape |
| Damage poll, idle | **~2–4 ms** |
| Damage poll, busy | **~3–8 ms** |
| Full screen, shm | ~530–890 ms |
| Full screen, protocol (`XReadDisplay`) | ~1070 ms |
| Full screen, `XGetImage` fallback | ~1600–3900 ms, returns 8-bit indices, needs the colormap |
| Sampled grid, every 8th row | ~78 ms (not needed any more, but measured) |
| Downscale 1/2 (C, box filter) | ~410–630 ms — the slowest thing in the loop after encode |

Per-rectangle overhead is real (roughly 1–2 ms each), so merge rectangles rather
than sending hundreds of tiny ones.

### The whole chain, measured

`ports/rust/agent-portable` builds a `pipeline` binary that runs capture ->
scale -> convert -> VP8 encode on the machine itself. Emulated R5000 Indy, one
processor, screen mostly static:

| stage | 320x256 (1/4) | 640x512 (1/2) |
|---|---|---|
| capture (forced full read) | 376 ms | 365 ms |
| downscale | 548 ms | 301 ms |
| convert to I420 | 87 ms | 308 ms |
| VP8 encode | 464 ms | 1823 ms |
| **total** | **1475 ms (0.68 fps)** | **2796 ms (0.36 fps)** |
| idle poll (nothing changed) | 4 ms | 15 ms |

Encode is 3.9x more expensive for 4x the pixels, so it is linear in area as
expected, and it dominates at 1/2. Divide by roughly 3 for a real Indy and
further for an O2; treat these as an upper bound.

**The clearest optimisation is the downscale.** It costs as much as the encode
at 1/4 because it reads the whole 1280x1024 canvas regardless of how little
changed. Scaling only the damaged rectangles, or fusing scale with the I420
conversion so the canvas is walked once, would take a large bite out of both.

The idle numbers are the important ones for a real session: a static desktop
costs 4-15 ms per frame to discover that nothing happened.

The 8-bit screen matters only for the fallback: `XGetImage` returns palette
indices with all masks zero, so the shim reads the 256-entry colormap and
expands. On an idle root window every colormap entry reads back 0,0,0, which is
correct and looks like a bug — it is an empty colormap, not a broken read.

### The trap that would have bitten in the field

**`XCloseDisplay` poisons every later ReadDisplay connection in the process.**
Found because the second `rd_capture_open` in one process failed with
`BadRequest, request 0.0`. `probes/reopen.c` runs one variant per process and is
unambiguous:

| teardown | result |
|---|---|
| destroy buf + `XCloseDisplay` | round 1 ok, rounds 2–3 **fail** |
| keep buf + `XCloseDisplay` | round 1 ok, rounds 2–3 **fail** |
| destroy buf, no `XCloseDisplay` | 3/3 ok |
| keep both | 3/3 ok |
| destroy buf, `close(ConnectionNumber(dpy))`, no `XCloseDisplay` | **3/3 ok** |

The extension's state is process-global and the close hook corrupts it. This is
not academic: the agent must reopen the display every time this server is
restarted, and without the workaround it would survive exactly one restart
before going blind for the rest of its life.

`rd_capture_close` therefore closes the socket by hand and never calls
`XCloseDisplay`, leaking the `Display` allocation once per reconnect. Verified
through the Rust `Drop` path as well: three build/drop cycles in a row work.

---

## What now exists in this repo

```
src/capture_shim.h     the capture interface, and why it is shaped that way
src/capture_shim.c     C implementation: damage / readdisplay / getimage paths,
                       shm canvas, palette expansion, box-filter downscale,
                       X I/O error handling via setjmp so a dead server does not
                       take the process with it (Xlib's default handler exits)
src/capture.rs         Rust side. Keeps the PowerPC agent's BANDS vocabulary so
                       session.rs needs no changes, and adds poll_rects() for the
                       finer answer.
probes/rawx.c          hand-rolled X11 setup, no Xlib — the probe that split the blocker
probes/xcapture.c      format and cost probe (byte order, XRD_READ_POINTER)
probes/xshmcap.c       shm + XShmReadDisplayRects: geometry, cost by shape, colormap
probes/sgicap.c        SGI-SCREEN-CAPTURE end to end
probes/capture_test.c  exercises capture_shim.c; modes: all | 0 | 1 | 2 | reopen
probes/reopen.c        the XCloseDisplay experiment, one variant per process
ports/rust/env.sh      sets the private RUSTUP_HOME/CARGO_HOME and build vars
ports/rust/hello/      minimal std smoke test for the target
src/input_shim.c       keyboard and pointer injection over XTEST, presenting the
                       same C interface input.rs calls on the Mac. Issues the
                       XTEST requests itself; see the note on SGI archives.
tools/fix-sgi-archive.py
                       repairs sh_info on SGI static archives so LLD will read
                       them. Not enough for libXtst, but the defect is real and
                       will show up again.
ports/rust/agent-portable/
                       the agent's modules, included from the PPC tree by #[path]
                       so they cannot drift, plus:
                         rustdesk-agent     THE AGENT, built from the PPC main.rs
                         testpeer           a client that speaks the real protocol
                         portable-selftest  json/convert/png/zstd/crypto/config/http/protobuf
                         capture-selftest   the capture module through its Rust API
                         pipeline [factors] capture -> scale -> convert -> encode, timed
                                            (defaults to 1/4 and 1/2)
ports/iris-run/iris-target/
                       a private CHD-capable iris build — the shared one lost the
                       chd feature. Use ./iris-target/release/{iris,iris-ci}.
ports/iris-run/gsh.py  marker-based telnet runner (see below)
```

### Building for the target

```
cd ports/rust/agent-portable
. ../env.sh                  # private RUSTUP_HOME/CARGO_HOME + SODIUM_LIB_DIR etc.
cargo +nightly build --release
```

If the link fails on an unresolved `-lrust_irix_compat`, `env.sh` was not
sourced.

### Build times, and where the build happens

**The build is entirely on the Linux host. iris plays no part in it.** This is a
cross-compile -- clang-18 plus `ld.lld-irix` targeting `mips-sgi-irix6.5` -- and
the numbers below were measured with no emulator running at all. The emulator is
only needed to *run* what comes out, and on current evidence real hardware would
be a better place to run it than IRIS (see §THE REMAINING BLOCKER).

Measured on this host (6 cores):

| | time |
|---|---|
| Full build after `cargo clean` — 44 crates including std, alloc, core, compiler_builtins, protobuf, sodiumoxide | **37 s** (279% CPU, 850 MB peak RSS) |
| Incremental after editing a Rust file | **6.4 s** |
| Incremental after editing a C shim | **7.3 s** |

One-time setup, on top of that:

| | time |
|---|---|
| Copy nightly into the private `RUSTUP_HOME` (1.6 GB) | about a minute |
| `patch-rust-sysroot.sh` + `mogrix patch-crates` | seconds |
| First `cargo build` (downloads the crate registry) | a few minutes, network-bound |
| zstd 1.5.6 for n32 | under a minute |
| libsodium, libvpx, mbedTLS for n32 | built in an earlier session; not timed |
| A CHD-capable iris, if you need to run anything | 5m33s |

What the build *does* need from an IRIX machine is `/opt/irix-sysroot` — headers
and shared libraries pulled off the 6.5.22m image. That extraction is already
done and is not part of a normal build.

Binaries land in `target/mips-sgi-irix6.5/release/`. On the guest they need
`LD_LIBRARYN32_PATH=/usr/sgug/lib32` for `libgcc_s.so.1`.

---

## Rust cross-compilation — what it took

`rules/methods/rust-cross.md` is accurate about the architecture and stale about
the details. Working through it:

1. **Private toolchain.** `ports/rust/rustup` is a copy of nightly
   `1.99.0 (1ed2df61a 2026-08-04)`; `ports/rust/cargo` is a fresh registry.
2. **`scripts/patch-rust-sysroot.sh` needed nine new patterns** for this
   nightly's std layout. All were added to mogrix alongside the old ones, so the
   script now handles both layouts:
   - `os/mod.rs` and `os/unix/mod.rs` — indentation changed in opposite directions
   - `sys/random/mod.rs` — the `unix_legacy` group membership changed
   - **`sys/pal/unix/os.rs` no longer exists**; `current_exe` now lives in
     `sys/paths/unix.rs`. `patch_file` used to die on the missing file, which
     silently stopped every later patch from running at all — it now reports and
     skips.
   - `sys/fd/unix.rs` — the cloexec cfg lists gained `qnx`, and the exclusion
     closes with three parens
   - `sys/fs/unix.rs` — IRIX has no `dirent.d_type`, so it must leave both
     `file_type` and `remove_dir_all`'s `is_dir`, and join the stat fallback and
     the `d_ino` group
   - `os/unix/process.rs` — **IRIX's `uid_t`/`gid_t` are signed**. The libc crate
     already groups irix with nto/qnx at `i32`; std's `UserId`/`GroupId` had to
     be told the same or every uid/gid call fails to typecheck.
3. **`mogrix patch-crates` hardcoded `~/.cargo`.** Changed to honour
   `CARGO_HOME`, which is the only way two efforts on one machine can avoid
   rewriting each other's crate sources.
4. **Pin protobuf exactly.** `protobuf = "3.0.0-alpha.2"` resolves to 3.7.2,
   whose `Message` trait does not match the checked-in codegen — 1903 errors.
   `=3.0.0-alpha.2` is required.
5. **libsodium-sys wants `SODIUM_LIB_DIR` and *not* `SODIUM_STATIC`**, which it
   now panics on. `SODIUM_SHARED` unset plus `SODIUM_LIB_DIR` is the working
   combination.
6. **`libgcc_s.so.1` is required and was not on the image.** It supplies the
   `_Unwind_*` symbols std's backtrace support references even under
   `panic=abort`. Copied to `/usr/sgug/lib32/` on the guest. For a shipped agent,
   either bake an rpath (`has-rpath` is true in the target spec) or ship it
   beside the binary — do not expect `LD_LIBRARYN32_PATH` to be set for a user.

Two loose ends worth knowing: `std::env::consts::OS` comes back **empty** on this
target, so anything switching on it will misbehave; and the release binaries
carry one `R_MIPS_REL32` relocation, which runs fine despite `irix-ld`'s
`fix-anon-relocs failed` warning (the missing `cross/lib/elf_utils.py`).

---

## Next steps, in the order they pay

1. **Encode at a reduced size.** The agent currently captures, converts and
   encodes the full 1280x1024, which measured ~6.1 s per VP8 frame on its own.
   `Capturer::scaled` exists and works; what is missing is threading a scale
   factor through `Video` — the `I420` and the encoder are built at
   `cap.width/height`, and `Video::band` converts native-resolution rows. This
   is the single biggest win available and it also cuts the capture volume that
   provokes the server wedge.

   Wire it to `image_quality` / `custom_image_quality` while you are there: they
   arrive in `session.rs` (search `image_quality`) and are currently logged and
   ignored.

2. **Check the colours.** Still unverified end to end, and the single most
   likely remaining bug. `convert.rs` was written for the Mac's **A,R,G,B**;
   IRIX hands us **A,B,G,R**. `--probe-display` shows the C shim and the Rust
   path produce *identical* planes, which proves they agree with each other and
   nothing about whether either is right for this byte order. Decode a delivered
   frame and look at it — red and blue swapped is easy to miss and miserable to
   find remotely.

3. **Use the rectangles.** `dirty_bands` throws away most of what the server
   said; `poll_rects` keeps it. Encoding only the changed regions is the obvious
   next saving, and it keeps reads small, which is what the emulated server
   wants.

4. **Cut the downscale cost** once it is in the path: it walks the whole canvas
   regardless of how little changed. Scaling only the damaged rectangles, or
   fusing the scale with the I420 conversion so the canvas is walked once, would
   take a large bite out of both.

5. **Keyboard injection is written but unproven.** The mouse path is verified;
   `rd_key`/`rd_key_char` have never had a real keystroke put through them,
   because the bare X server has no window manager and nothing focused to type
   into. Start `4Dwm` or an `xterm` and drive it from `testpeer`, or use
   `--probe-keys X Y`.

6. **The clipboard and cursor paths are compiled but untested on IRIX.**
   `cursor.rs` matters less than it did — `cursor_embedded` is honoured, so the
   agent should never need to send a shape — but the code that decides that has
   not been exercised against a peer.
6. **Input, clipboard, cursor** shims: `XTEST` is advertised, and cursor work is
   mostly retired by `cursor_embedded`.
7. **The remaining portable module is `sys.rs`**, plus `api.rs`, `lan.rs`,
   `rendezvous.rs` and `session.rs`, which were not attempted tonight.

---

## Operating the emulator — the mechanics that cost time

### Boot

**The shared `~/repos/iris/target/release/iris` no longer has CHD support.** The
other session rebuilt it mid-evening with only `tlbvmap`, and it now refuses the
image with "CHD image support not compiled in". Rather than rebuild in their
target directory — they are actively editing that source — this session built a
private copy:

```
cd ~/repos/iris
CARGO_TARGET_DIR=<agent repo>/ports/iris-run/iris-target \
  cargo build --release --features lightning,rex-jit,r5k,chd
```

which took 5m33s and left `ports/iris-run/iris-target/release/{iris,iris-ci}`.
**Use those.** Check `iris: build features:` on the first line of the log — if
`chd` is missing, that is why nothing boots.

```
cd ports/iris-run
DISPLAY=:1 ./iris-target/release/iris --config iris.toml --ci --ci-display
```

**`iris-ci start` is required.** Under `--ci` the CPU thread is created paused;
without `start` the machine sits there doing nothing and `console.log` stays
empty. This cost 20 minutes tonight — it looks exactly like a hung boot.

```
export IRIS_SOCKET=/tmp/iris-rdagent.sock        # NOT the default /tmp/iris.sock
iris-ci start
iris-ci serial-wait --timeout 900 "login:"
```

Boot to a usable telnet takes about 5 minutes; **telnetd answers well before the
serial console prints its login banner**, so do not use the banner as the
readiness test — check port 2324 instead.

### Talking to the guest

Use **`ports/iris-run/gsh.py`**, not `irixsh.py`. It brackets every command with
a unique marker, so output can never be attributed to the wrong command —
`irixsh.py` strips the echoed line heuristically and slides output by one
command, which silently produced wrong answers for several commands tonight.

```
python3 ports/iris-run/gsh.py 'uname -a' 'ls /usr/sgug/lib32'
GSH_TIMEOUT=600 python3 ports/iris-run/gsh.py -f commands.txt
```

Two things it handles that matter: it widens the tty to 1000 columns, because at
80 the echoed command wraps and inserts spaces mid-marker; and it takes the
*last* marker occurrence, because the guest echoes the command before running it.

`/root/tmo` on the guest is a watchdog — `tmo SECONDS command...` — since IRIX
has no `timeout(1)` and a hung X client otherwise wedges the shell it came from.

### Moving files in

```
# host
cd <dir with the files>; python3 -m http.server 8099
# guest
/usr/nekoware/bin/wget -q http://192.168.0.1:8099/<file> -O /tmp/<file>
```

### Shutting down

`/etc/halt` **prompts** `Halt IRIS ? (yes/no)[no] :` and does nothing if the
answer never arrives — which is why it looked broken over telnet. Use:

```
python3 gsh.py 'echo yes | /etc/halt'
# wait for "Okay to power off the system now." in console.log
kill -TERM <iris pid>
```

---

## Disk handling — non-negotiable

Dani wants changes **applied to the CHD**, not left in copy-on-write.

**A headless/`--ci` session never folds the diff.** The auto-fold is wired only
into `iris-gui`'s close path. `iris-ci quit` exits 0 and silently leaves
everything in `<base>.chd.diff.chd`.

Fold by hand, with iris stopped:

```
chdman copy -i ~/Indy-IRIX65_dev.chd.diff.chd -ip ~/Indy-IRIX65_dev.chd \
            -o merged.chd -c lzma,zlib,huff,flac -hs 4096
```

Pass `-c` and `-hs` explicitly to preserve codecs and hunk size. **Then verify
before swapping**: extract, carve the root partition
(`bs=512 skip=266240 count=8122368`), `xfs_repair -L` the *copy*, mount, compare
files. Then `mv` over the base and `chmod 700` (chdman writes 664). Keep the
backup.

An IRIX XFS root **always** reports "valuable metadata changes in a log", even
straight after a clean `/etc/halt`. That is not evidence of an unclean
shutdown. `xfs_repair -L` is fine on a throwaway copy and must never be run on
the real image.

### What changed on the disk this session

Folded into `~/Indy-IRIX65_dev.chd`:

- `/usr/sgug/lib32/libgcc_s.so.1` (new, 117728 bytes) and the `libgcc_s.so`
  symlink — every Rust binary needs it for `_Unwind_*`.
- `/root/restart-x.sh` (new) — the tested X recovery procedure.
- `/root/tmo` (new) — the watchdog helper.
- `/tmp` was emptied of the session's test binaries before shutdown.
- Ordinary system churn: `/var/adm/SYSLOG`, wtmp, the XFS log.

`/etc/X0.hosts` was already there from the previous session and turned out to be
irrelevant; it is harmless and was left alone.

**The fold was verified three ways** and then the image was booted from:

- `xfs_repair -L` on a throwaway carve of the merged image completed clean
  (mounting was not possible — this box has no passwordless sudo).
- The three new files' bytes were found in the carved filesystem, `libgcc_s.so.1`
  by matching a 64-byte window from the middle of the source file.
- The merged image was booted, all three files were present with the right sizes
  and checksum, X was restarted with the script, and the full Rust capture
  self-test and the pipeline benchmark ran against it.

Two folds happened, one per session half. Artifacts left beside the image, all
safe to delete once you are happy:

```
Indy-IRIX65_dev.chd.prefold2-20260819          the base before the second fold, 1.1 GB
Indy-IRIX65_dev.chd.diff.chd.folded-20260819   first diff, 47 MB
Indy-IRIX65_dev.chd.diff.chd.folded2-20260819  second diff, 61 MB
```

The second fold added `/root/agent-restart.sh` (stop the agent, refetch it,
restart X, start it listening — long command lines corrupt themselves over
telnet, so this exists as a script). It was verified the same way: booted from
the folded image, all helper files present, and a full agent-plus-peer session
run against it.

The verification boot's own diff (SYSLOG, wtmp, nothing else — the test binaries
were deleted before halting) was **discarded** rather than folded, so the base is
exactly the verified image and no sidecar is pending.

Disk cost of this session's working set, for when space gets tight:
`ports/rust` 2.1 GB (private toolchain + registry), `ports/iris-run/iris-target`
1.2 GB (the private CHD-capable iris build).

---

## Mistakes not to repeat

- **A wedged server looks exactly like a broken client.** Before blaming your own
  binary against any service, prove the service answers *someone*. `rawx.c` did
  in one run what four ruled-out hypotheses could not.
- **`iris-ci start` is not optional** under `--ci`. An emulator that has not been
  started is indistinguishable from a slow boot.
- **iris rebinds whatever `ci_socket` it is given**, deleting the existing
  socket file first. On a shared machine that silently takes over another
  session's emulator.
- **`killall` on IRIX is SysV: it kills ALL processes.** Use pids from `ps -ef`.
- **`pkill -f <pattern>` matches its own shell.** Use `pgrep -x` or explicit pids.
- **`console.log` is cumulative across runs.** Rotate it, or grep only the tail.
- **`/etc/halt` asks a question.** Pipe `yes` into it.
- **Guest command lines wrap at 80 columns** over telnet and corrupt anything
  parsing the echo. `gsh.py` widens the tty; if you write your own driver, do the
  same.
- **Write probes cheapest-first, unbuffered, announcing each step.**
- **Symbols resolve at link time and fail at run time on IRIX.** Always link
  `-lpthread`; add `-lgcc_s` if anything uses `__int128` or unwinding.
- **`relocations in generic ELF (EM: 8)`** means the *host* linker got MIPS
  objects.
- **`pkill -x iris` kills every iris on the machine, including the other
  session's.** I did this and took down a 4h52m-old emulator belonging to the
  rust-irixlibstd session. Kill the pid whose `/proc/<pid>/cmdline` names *this*
  config, and note that `ps | grep "iris --config"` matches the bash wrapper as
  well as the emulator — killing the wrapper leaves the emulator running, which
  is how two instances ended up fighting over the same CHD and port forwards.
- **IRIX has no `pkill`.** `pkill -x rustdesk-agent` silently does nothing, so
  the old agent keeps the port and the next one exits with EADDRINUSE. Use
  `for p in \`ps -e | grep name | awk '{print $1}'\`; do kill -9 $p; done`.
- **Guest command lines longer than the tty width corrupt themselves**, even
  with `stty columns 1000` — backticks and semicolons come back mangled and bash
  reports a syntax error on something you did not write. Put anything long in a
  script, fetch it with wget, and run that. `/root/agent-restart.sh` is one.
- **Do not trust a self-test that fails on working code.** Two "failures" in the
  first portable self-test run were wrong assertions in the test — `json::field`
  returns values still quoted, and `escape_into` writes the quotes.

---

## Repo state

### The PowerPC tree has been edited

`../rustdesk/rustdesk-ppc-agent` is no longer untouched. The changes are all
additive platform gates plus two small behaviour fixes, and none of them can
change what macOS builds:

```
src/session.rs   gates widened to any(macos, irix); poll(2) pacing instead of
                 SO_RCVTIMEO on IRIX; platform string "Linux" on IRIX
src/main.rs      gates widened to any(macos, irix)
src/input.rs     gates widened to any(macos, irix) so the shim is used
src/sys.rs       added wait_readable (IRIX only)
```

Backups of the originals were left in /tmp during the session; if the Mac build
needs to be checked, `any(macos, irix)` reduces to `macos` there by definition,
and `wait_readable` is behind `cfg(target_os = "irix")`.

`~/repos/mogrix` on `danifunker-ports`, uncommitted:

```
M .gitignore                                   anchored /lib/ and /lib64/
M compat/include/mogrix-compat/generic/time.h  CLOCK_MONOTONIC -> CLOCK_SGI_CYCLE
M cross/bin/irix-ld                            LLD path was hardcoded to /home/edodd
M pyproject.toml                               mcm-engine pinned to a local path
M scripts/build-runtime-objects.sh             -O2 -fno-builtin for safe_mem
M scripts/patch-rust-sysroot.sh    NEW TONIGHT nine patterns for the 2026-08 std
                                               layout, plus tolerate a missing file
M mogrix/crate_patcher.py           NEW TONIGHT REGISTRY_BASE honours CARGO_HOME
?? cross/lib/{dso_handle,safe_mem}.c, irix-shared.lds
?? rules/packages/{libsodium,libvpx,mbedtls}.yaml
?? patches/packages/{libvpx,mbedtls}/
```

**Still missing from a fresh mogrix clone** (Dani should ask unxmaal):
`compat/runtime/*` (gitignored) and `cross/lib/elf_utils.py`. `soft_float_stubs.c`
is a reconstruction that **aborts by name rather than forwarding** to IRIX's
`__q_add`/`__q_mul`; `safe_mem.c` is also a reconstruction.

The `mbedtls`/`libsodium`/`libvpx` rules still have **not** been through a mogrix
session with the MCP knowledge server connected, which its `CLAUDE.md` requires
(`add_rule`, `report_error`). Flag for Dani; do not fake it. The stray empty file
`~/repos/mogrix/a` is Dani's to remove.
