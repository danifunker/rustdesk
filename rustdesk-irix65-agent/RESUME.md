# RESUME — RustDesk agent for IRIX (SGI MIPS)

Pick-up point. Last updated 2026-09-17, end of the session that **ran it on a
real O2** — see §REAL HARDWARE, which supersedes a good deal of what the
emulator taught us. Before that, 2026-08-24 **pressed the
buttons and made a package**. The Motif panel's Start, Stop and Apply have now
been clicked by injected input and do what they say; typing into a text field
works, which is the first time a keystroke has ever gone through `rd_key_char`
to a real focused window; and there is a `.tardist` that installs with `inst`
on a machine that has never had a cross toolchain or SGUG-RSE on it. See
§THE BUTTONS WORK and §THE PACKAGE.

`RESUME-PROMPT-PIPELINE.md` is the handover, and it opens with the two things
that changed underneath this work after it was finished: the branch was rebased,
so `dist/` names a commit that no longer exists, and the shared PowerPC tree has
since moved — including inside IRIX `cfg` blocks. **Read that before you build
anything**, because the next build will not be the binary that was tested.

Before that, the session of 2026-08-23 **made it fast
enough to use**. Ordinary interaction — a pointer moving, text appearing — runs
at **5-20 fps on the emulator**, which is three times that on a real Indy. A
full-window repaint is still 1.3-1.9 fps and that is close to the floor of this
design; §Where the wall is says why and what would move it. Before this session
the whole thing ran at 0.03 fps.

Read §How fast is it, honestly before quoting any single number: the two ends of
the workload range differ by an order of magnitude, and the host these were
measured on is shared and loaded.

Before that, the session of 2026-08-19 **got the agent itself running**: a peer
connects over the real protocol, logs in, receives VP8 video, and mouse
injection works.

Capture is built and verified on hardware, in C and in Rust. Rust std builds for
`mips-sgi-irix6.5`, the whole agent compiles and links, and `rustdesk-agent`
serves a real session at a rate a person could use.

**A logged-in session is proven on hardware; a logout is not.** Every
measurement in the emulator sections below was taken against a bare
`Xsgi :0 -bs -c`, because starting xdm on the emulator wedges the X server. On a
real O2 a *logged-in* 4Dwm desktop is fine: xdm and the toolchest run,
`xdpyinfo` answers before and after a full capture run, and capture works
against it. **Logging out still wedges the server**, on real hardware, with the
same frozen-CPU signature. See §REAL HARDWARE.

---

## REAL HARDWARE — an O2, and what it changed

`sgio2`, 192.168.99.41, joined the network 2026-09-17 and is the first real SGI
this port has ever run on. Everything before this was the emulated Indy.

```
IRIX sgio2 6.5 6.5.22m IP32     the SAME release as the dev image
CPU      MIPS R10000 rev 2.6, 195 MHz, 1 MB L2
memory   448 MB
graphics CRM (O2), not the Indy's newport/REX3
screen   1280x1024, depth 8 pseudocolour -- identical to the dev image
root     blank password; reached over rexec (port 512). No key was installed.
```

**The display side carried over untouched.** `SGI-SCREEN-CAPTURE`,
`READDISPLAY`, `MIT-SHM`, `XTEST` and `XKEYBOARD` are all present on CRM
graphics, and ReadDisplay returns stride 5120 — 32 bpp despite the 8-bit
screen, exactly as §Capture predicted, so the colormap path never engages.
Nothing in the capture design needed changing for a different graphics family.

**A logged-in desktop does not wedge; a logout still does.** The O2 runs xdm,
`Xsgi`, 4Dwm and a toolchest, with the same argument list §A REAL LOGIN SESSION
quotes and the same absent `-auth` — so the host-based `/etc/X0.hosts` finding
holds on hardware too (`xhost` reports `LOCAL:`, `localhost.localdomain`,
`sgio2`). `xdpyinfo` answered instantly before a full `--probe-display` run and
again after it, and capture against that live desktop worked throughout.

**But the wedge is not emulator-only, and this file said so for a few hours on
the strength of the paragraph above.** Session captured, agent working, user
logs out — and `Xsgi` is left with its CPU time frozen at 3:16 across ten
seconds, answering nobody; `xdpyinfo` hangs rather than failing, and four of
them stacked up before anyone noticed. That is the signature in
`docs/ISSUE-xdm-wedge.md` exactly, on an O2 rather than under IRIS. What is
proven on hardware is the *logged-in* case. The logout path is still the open
one, and it is worse than it looked, because on the emulator it could be blamed
on the emulator.

**Anything that probes X must bound its wait.** A wedged server accepts the
connection and never replies, so `xdpyinfo` does not fail, it hangs. The boot
script's readiness probe learned this the hard way; §Mistakes not to repeat.

### The package did not work, and the install test could not have told us

A stock O2 has no `/usr/sgug`. The development guest does — SGUG-RSE is
installed in it — and `iris-install-test.sh` installs into that guest. So the
claim this repo has made since §THE PACKAGE, that the `.tardist` installs on a
machine that has never had a cross toolchain or SGUG-RSE, was only ever tested
against a machine that had exactly that. It was self-confirming.

On the real thing the agent installed cleanly (`inst` rc=0, no conflicts, the
hinv compatibility check proposed removing nothing) and then exited **1 with
zero bytes of output** for every argument, `--help` included. The cause:

```
rld: Error: unresolvable symbol in /usr/sbin/rustdesk-agent: compressBound
rld: Fatal Error: this executable has unresolvable symbols
```

`compressBound` arrived in zlib 1.2.0. IRIX 6.5 ships zlib, but shipped more
than one across its life: the sysroot pulled from the 6.5.22m build image is
1.2.1 and has it, a stock 6.5.22m O2's `/usr/lib32/libz.so` does not. `build.rs`
linked `-lz` on the strength of a comment reading "which IRIX 6.5 ships itself".

**Two things are worth carrying forward from this, beyond the fix.**

The first is where rld says it. Those lines go to **`/var/adm/SYSLOG`, not
stderr**, so the failure presents as a silent `exit 1` and every obvious
diagnostic — run it again, redirect stderr, check the file is there, check it is
n32 — says nothing at all. `par -s -SS` on the process is what found it: the
trace ends `write(4, "<11>...rld[84748]", 123)` then `exit(1)`, and `<11>` is
syslog user.err. **On IRIX, when a dynamic executable dies silently, read
SYSLOG before anything else.**

The second is that `build.sh`'s run-time dependency check could not have caught
it. It lists `readelf -d` Shared library entries, and an unresolved symbol names
no library — so it appears in that list nowhere. The check is necessary, not
sufficient; there is now a note in `build.sh` saying so.

**The fix is static linking**, `cargo:rustc-link-lib=static=z`, against a zlib
**1.3.2** built for n32 in `ports/work/zlib-1.3.2` and staged to
`$SGUG_STAGING/lib32/libz.a` — the same treatment libvpx and mbedTLS get.
Shipping a `libz.so` beside the agent also works and was tried first, but it
means carrying SGI's 2003-vintage 1.2.1, old enough to predate the fixes for
CVE-2018-25032 (a `deflate` bug, and `png.rs` is a deflate caller) and
CVE-2022-37434. Static linking takes the machine's zlib out of the question.
After it, `readelf -d` on the agent names only `libXext`, `libX11`, `libc`,
`libm`, `libpthread` and `libgcc_s.so.1` — all stock, plus the one shipped file.

### Performance: better than the estimate, and the downscale disappears

`--probe-display` at full 1280x1024 against the live 4Dwm desktop, against the
emulator's table at half resolution:

| | emulated Indy @ 640x512 | O2 @ 1280x1024 |
|---|---|---|
| pixels | 0.33 Mpx | 1.31 Mpx (4x) |
| capture | 365 ms | 191 ms |
| downscale | 301 ms | not in the chain |
| convert to I420 | 308 ms | 260 ms |
| VP8 encode | 1823 ms | 1256 ms |
| **total** | **2796 ms** | **1707 ms** |
| idle poll | 15 ms | **0 ms** |

Four times the pixels in 61% of the time: about 5.8x per megapixel on encode,
4.7x on convert, 6.5x on the chain as a whole. §The whole chain said "divide by
roughly 3 for a real Indy and further for an O2" — the real figure is better
than that, and it is measured at full resolution where the emulator needed to
downscale. The **idle cost is 0 ms**, which matters more than the headline: a
static desktop costs nothing to discover nothing happened.

The tuning sweep says the config in use is not the best available here:
`static_threshold: 30000` encodes in 1114 ms against 1256 ms for the 1000 in
use, and `profile 2` (no loop filter) in 1129 ms. `screen_content` 1 or 2 is a
clear loss on this hardware — slower (1468 ms) *and* double the bytes (19033
against 9580). None of this is wired up; §PERFORMANCE's tuning is still the
emulator's.

### The install prefix moved to /usr/local, 2026-09-17

It installed into `/usr` until then, which is SGI's own space. It is now
`/usr/local/sbin/rustdesk-agent`, `/usr/local/lib/rustdesk-agent/` for the
helper and libgcc_s, and `/usr/local` is the default `install.sh` prefix. Paths
written `/usr/sbin/...` elsewhere in this file are the historical record of
sessions that ran before the move; they have not been rewritten.

**The Toolchest fragment did not move and cannot.** `/usr/lib/X11/system.chestrc`
ends with `sinclude /usr/lib/X11/app-chests`, so the desktop reads exactly one
directory. The fragment stays at `/usr/lib/X11/app-chests/RustDesk.chest`; only
the path inside it, which names the panel, changed.

**This needs a rebuild, not just a move.** The rpath is baked in at link time
(`RD_RPATH` in `ports/rust/env.sh`), so a binary linked for `/usr` and copied to
`/usr/local` cannot find its libgcc_s. `install.sh` writes a wrapper for any
prefix that does not match `DEFAULT_PREFIX`, which is how `-p` has always
worked; the point of matching the default is that no wrapper is needed and the
installed file is the real ELF. Verified on the O2: `file` reports an N32
executable rather than a shell script, and `--show-id` returns 0 with
`LD_LIBRARYN32_PATH` unset.

Both runtime lookups search the old location after the new one — the helper's
agent search and the panel's `find_helper` — so a new build still works against
a machine installed before the move.

### Still unproven, even now

- **A peer session on hardware.** Everything here is `--show-id`, `--show-key`,
  `agent-helper.sh status` and `--probe-display`. No client has connected to the
  O2, because that needs the three values §Next steps item 0 asks for.
- **The agent surviving the X server restart xdm does between logins.** The
  desktop was live throughout and never restarted.
- **Damage volume on a desktop in use.** The probe measures a mostly static
  screen; §A REAL LOGIN SESSION's `MAX_RECTS` question is open.

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

emulator        ~/iris-upstream/target/release/{iris,iris-ci}   upstream at 02c4e155,
                which has the hostr readback fix. --cpu is a RUNTIME option now
                (--cpu r5000); the CPU is no longer a build feature, and there is
                no private build any more. Do NOT go back to an older one.
                ~/repos/iris has uncommitted work that is not ours: do not touch.
disk            ~/Indy-IRIX65_dev.chd            IRIX 6.5.22m, R5000, /usr/sgug populated
backup          ~/Indy-IRIX65_dev.chd.bak-before-first-write   pristine, keep
run config      ports/iris-run/iris.toml         ci_socket is now /tmp/iris-rdagent.sock
nvram           ports/iris-run/nvram-irix65.bin  has console=d; do not lose it
shells          ports/iris-run/irixsh.py         telnet driver (superseded, see gsh.py)
                ports/iris-run/gsh.py            marker-based telnet runner — interactive
                scripts/ci-lib.sh  guest_run()   SERIAL console — anything automated
package         scripts/release.sh               build -> gendist -> .tardist + .tar.gz
                scripts/iris-install-test.sh     install it in the guest and run it
                docs/PACKAGING.md                the whole pipeline, and why
harness         ports/iris-run/serve.sh          serve the build on :8099
                ports/iris-run/guest/*.sh        what runs on the guest; fetch.sh
                                                 pulls the lot in one command
telnet/ssh      host 2324 -> guest 23,  host 2222 -> guest 22
guest net       192.168.0.2, gateway/host 192.168.0.1

mogrix fork     ~/repos/mogrix, branch danifunker-ports (remote danifunker-origin)
libvpx          BUILT BY HAND in ports/work/libvpx-1.13.1 and copied to staging,
                and it now carries a local performance patch — see §PERFORMANCE
                and patches/. A stock libvpx costs about half the frame rate.

layout          this tree lives at rustdesk/rustdesk-irix65-agent, beside
                rustdesk-ppc-agent, on the `vintage-agents` branch. The agent's
                portable modules are included from the PPC tree by #[path], so
                the two must stay siblings -- the paths are relative and a clone
                of one without the other will not build.
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
| **The agent** | `rustdesk-agent` builds from the PPC `main.rs` and serves a session: `testpeer` logs in over the real protocol and receives continuous VP8 video. |
| **Input injection** | XTEST, via `src/input_shim.c`. Absolute moves, relative moves and display clamping all verified by `--probe-live`. |
| **Rust process spawning** | `std::process::Command` spawn/kill/wait works — verified by having the self-test start `xclock` to generate damage. |
| **The colours, end to end** | Three xterms with red, green and blue backgrounds, captured, downscaled, converted, VP8-encoded, and **decoded by the peer**: 255/1/5, 0/254/2, 2/0/255. The byte order was wrong before this and is the reason the check exists. |
| **The stream tracks the screen** | 100 s against a moving desktop, then the decoded frame compared with a fresh capture: mean absolute difference **2.40 of 255**, and every large difference inside the one region still moving. This is what makes the active map safe to rely on. |
| **Coordinate scaling** | A peer on a 1/2 session asked for a pointer move at (100, 100); the pointer landed at native (200, 200). |
| **`image_quality`** | The peer's dial reaches the picture size: Best -> 1280x1024, Balanced -> 640x512, Low -> 320x256, each announced with a `SwitchDisplay` before the first frame in it. |
| **The panel's buttons** | Start, Stop and Relay's Apply clicked by injected input: the agent appears, the agent goes, and `relay_server` reaches the config. `guest/gui-press.sh`. |
| **Keyboard injection** | `rd_key_char` typed `relay.press.test` into a focused Motif TextField, sixteen characters, all correct. First keystrokes ever delivered to a real window here. |
| **The package installs** | `inst -f` in the guest reports success, `versions` lists the product, and `/usr/sbin/rustdesk-agent --show-id` prints the ID **with `LD_LIBRARYN32_PATH` unset** -- so the rpath works and the machine needs nothing else. |
| **VP8 decode on IRIX** | `encode::Decoder` / `vpxdec_*`. 129 of 129 frames decoded in a live session; libvpx's VP8 decoder works on this big-endian 32-bit target too, not just its encoder. |

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
  display 'Display' 640x512 at 0,0, online true
  video frames   126 (1 key), 415629 bytes total
  first frame    5434 ms after login
  average        3298 bytes/frame, 1.40 fps over 90.1 s
VERDICT: the agent is serving video to a peer.
```

(That is after §PERFORMANCE. The same line on 2026-08-19 read `video frames 1 (1
key)` at 1280x1024, with the first one arriving 23 seconds after login.)

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

## THE WEDGE UNDER CAPTURE LOAD — fixed upstream, kept for the evidence

**This one is closed.** Upstream iris `02c4e155` ("fix hostr readback issues")
corrects the REX3 host-read path, and the X server no longer wedges under our
capture traffic. Everything below is the record of what it looked like, kept
because a *second* wedge is still live — the one xdm's visual-login rendering
provokes, see §A REAL LOGIN SESSION — and it has the same signature.

Fixing it also exposed three faults of our own that it had been masking, all
three ours and all three found by measuring rather than reading: a leaked shm
segment per failed open (80 orphans, ~400 MB on a 256 MB guest), a leaked file
descriptor per dead connection (1309 consecutive `XOpenDisplay failed` from one
agent while a fresh process captured perfectly), and `display_size()` building an
entire capture context on every pass of the message loop. **When something looks
like a platform problem, measure the agent first.**

What it used to look like:

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

`docs/REX3-WEDGE-PROMPT.md` is a self-contained handover for a session working
on iris: the evidence, the reproduction, what is already ruled out, the pointers
into `src/rex3.rs`, and the boundaries around the shared machine.

**For the agent:** less capture volume is worth having regardless, and is the
next piece of work. See §Next steps.

### Not CPU-specific

Rebuilt iris without the `r5k` cargo feature, so the guest comes up as
`MIPS R4400 Processor Chip Revision: 4.0` (66 MHz IP22) instead of R5000. Same
image, same disk, same wedge, same signature — the only difference was the first
frame arriving at 49 s instead of 23 s, which is what the slower CPU predicts.
That needed a separate build at the time. It does not any more: `--cpu r4400` /
`--cpu r5000` is a runtime option on upstream iris, and the private per-CPU
builds are gone. `hinv` on the guest is still how you check which one you got —
the `build features:` banner never said.

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

**A second crash mode, seen on the R4400 run:** after the server wedged, the
agent retried `rd_capture_open` every retry interval, and then died with

```
memory allocation of 141320 bytes failed
```

141320 bytes is exactly the size of the keyframe it had just sent, and the guest
has 256 MB, so something is consuming memory across failed reconnects rather
than the frame itself being too large. One candidate is by design and worth
checking first: `rd_capture_close` deliberately never calls `XCloseDisplay` (see
the note there), so every *successful* open leaks a `Display`. That is meant to
be a handful of allocations over a session's lifetime, not a per-retry cost, but
a reconnect storm against a wedged server is exactly the case that turns "rare"
into "every few seconds". Measure the RSS across retries before assuming.

Both crash modes are the same work item: **the agent must survive its X server
dying.** On this platform that is not an edge case.

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

## PERFORMANCE — 0.03 fps to 1.3

The agent worked and was unwatchable: five frames in 182 seconds, all five
keyframes at 138 KB each. It now serves 114-126 frames in 90 seconds against a
deliberately busy screen, and a small change — a caret, a clock hand, a line of
text — arrives in about **100 ms**.

Measured under a real peer on the emulated 66 MHz R5000 Indy, 1280x1024
framebuffer, with an `xclock` ticking and an `xterm` scrolling `date` and a
directory listing every two seconds. Divide by roughly 3 for a real Indy; an O2
is faster again.

```
before   video frames   5 (5 key), 694064 bytes over 182 s  = 0.03 fps
after    video frames 126 (1 key), 415629 bytes over  90 s  = 1.40 fps
```

Per frame, from the agent's own `FrameTimes` line (`-v`):

```
small change   frame:  2 band(s),   4/1280 MB, probe   2, conv   0, enc  95  =  104 ms
window redraw  frame: 27 band(s), 436/1280 MB, probe 106, conv 140, enc 283  =  541 ms
new peer       frame: 64 band(s), 1280/1280 MB, probe 648, conv 445, enc 3557 (KEY) = 4672 ms
```

### What actually moved, in the order it paid

**1. Encode what the peer is being sent, not the whole framebuffer.**
`Video` now carries a scale factor; `I420` and the encoder are built at
`cap.width/scale`, and `PeerInfo` reports that size rather than the
framebuffer's. Wired to the peer's `image_quality`, which until now was logged
and ignored: Best is 1/1, Balanced 1/2, Low 1/4. VP8's cost is per macroblock,
so this is linear in area and it is the single biggest lever.

Because the peer is told the truth about the size it will receive, its canvas,
its pointer and the video all agree and no client behaviour has to be taken on
trust. The price is that peer coordinates are in that space:
`Injector::set_display_scale` multiplies them back up and
`cursor::Tracker::set_scale` divides the other way. Verified against a running
agent — the peer asked for (100, 100) on a 1/2 session and the pointer landed at
native (200, 200).

**2. Encode only the macroblocks that changed.** `VP8E_SET_ACTIVEMAP` takes one
byte per 16x16 macroblock, and the SGI-SCREEN-CAPTURE damage rectangles say
exactly which those are. A frame that used to pay for 1280 macroblocks to
discover that four had moved now pays for four.

This is **only sound because the damage report cannot miss a change** — an
inactive macroblock keeps its pixels for ever, so an unreported change would be
permanent rather than late. That is why it is IRIX-only: the Mac's sampled
checksum is exactly the case it would break.

**3. A libvpx patch, because libvpx checked the map too late.** It honours the
active map inside `evaluate_inter_mode()`, by which point the realtime picker
has already run the predictor setup, the dot-artifact check (SAD over Y and both
chroma planes), the skin-map lookup and the near-MV search — for a macroblock
the caller said not to code. Exiting at the top of
`vp8cx_encode_inter_macroblock()` instead, three interleaved A/B rounds on the
target:

| frame size | stock | patched |
|---|---|---|
| 320x256 | 51/50/51 ms | 39/34/37 ms |
| 640x512 | 166/172/191 ms | 95/91/94 ms |
| 1280x1024 | 753/708/712 ms | 309/348/340 ms |

That is the cost of a frame whose active map selects **nothing at all** — the
floor under every frame however little changed. At 640x512 it took the ceiling
from 5.8 to 10.5 fps. `patches/libvpx-vp8-active-map-early-out.patch`, and the
same patch is now listed in mogrix's `rules/packages/libvpx.yaml`.

**4. VP8 profile 3: no loop filter, no sub-pixel motion.** The profile is not a
feature level — every VP8 decoder must handle all four — it is a set of encoder
cost decisions. The loop filter is a pass over every pixel of every frame whose
job is hiding block edges in natural video; on a desktop it is a whole-frame
cost paid to slightly blur the text someone is trying to read. Measured at
640x512 on a small change: **452 ms at profile 0 against 246 ms at profile 3**,
and the full-frame case no slower. `Tune::profile`, IRIX-only via `video_tune()`.

**5. The keyframe feedback loop, cut.** `KEYFRAME_INTERVAL` is 10 s; frames took
36 s; so every frame was older than the interval and every frame was forced to
be a keyframe — several times the cost of the inter frame that would have done,
which made the next frame later still. On IRIX the wall-clock rule is gone.
Measured directly: nine keyframes in 90 s at 3.7 s each is 33 seconds of a
90-second session, over a third of it, spent on insurance.

What replaces it is a **rolling refresh through the active map**: while the
screen is still, a few macroblock rows go back through the encoder each tick, so
a block the static threshold left uncoded is corrected within about fifteen
seconds of quiet, at a fiftieth of a keyframe's price. It laps once and stops,
so a genuinely static desktop still costs nothing.

**6. The settle repaint, deleted on IRIX.** The Mac re-reads a slice of VRAM on
a timer because its change detection is a sampled checksum that can miss. This
server reports every damaged rectangle and delivers the pixels with the report,
so there is nothing to repair — and the repair was not free: a lap is 64 forced
whole-band ReadDisplay round trips, 64 band conversions, and a forced keyframe
with them.

**7. Downscale fused with the colour conversion, over the damage.**
`Capturer::to_i420_rect` walks the canvas once instead of twice with a full-size
ABGR intermediate in between, takes a destination rectangle so a frame costs
what the damage costs, and has a hand-unrolled kernel for the 1/2 case. The
general path reaches each source pixel through two loops whose trip count is a
runtime value, so nothing unrolls; at four iterations apiece the
compare-and-branch is a large fraction of the work. Live, on a scrolling xterm:
**260-490 ms before, 140-230 ms after**, for byte-identical output.

**8. `MAX_RECTS` 64 -> 256.** A busy xterm reported more rectangles than 64, and
the bounding box the shim merged them into covered 434 of the frame's 1280
macroblocks where the real change was nearer 180 — a third of the conversion and
a third of the encode, on exactly the frames that were already the expensive
ones.

### The colours were wrong, and now are not

`convert.rs` reads **A,R,G,B**, which is the Mac's framebuffer. IRIX's
ReadDisplay hands back **A,B,G,R**. Red and blue were swapped on every pixel,
and it had survived two sessions of checking because the C shim and the Rust
reference agreed with each other and both were wrong.

Settled by measurement, not by reading. `xsetroot -solid red`, then count
sampled pixels by which byte is lit: **byte 3 lit on 5119 of 5120, byte 1 lit on
none**. Fixed in `convert.rs` and `png.rs` (screenshots had it too) via a `CH`
constant, and `to_i420_rect` has the right order built in.

Then verified through the whole chain rather than by inspection. `testpeer` now
carries a VP8 **decoder** (`encode::Decoder`, `vpxdec_*` in `vpx_shim.c`) and
writes the frame it decoded as a PPM. Three xterms with red, green and blue
backgrounds, read back out of that PPM on the guest:

```
red xterm    255 001 005
green xterm  000 254 002
blue xterm   002 000 255
```

Capture, byte order, downscale, conversion, VP8 encode and VP8 decode, correct
end to end. **This is the check to repeat after any encoder change** — "still
decodes" is not the same claim as "still looks like the screen".

### And the peer's picture really does track the screen

The active map is the part of this design that could fail silently, so it is
checked directly. `perfprobe verify` reads what the peer decoded, captures the
same screen now, converts it the same way, and reports the difference. After a
100-second session against a moving desktop:

```
mean absolute difference 2.40 of 255
7794 pixels (2.38%) differ by more than 24
worst macroblock at 48,32: mean 93.0
```

The worst block is inside the `xclock`, which was still ticking; 2.38% is very
nearly exactly the clock's share of the frame. A macroblock that had rotted
would show up as a cluster of large differences somewhere else, and there is
none.

### Three more, and where the wall is

A second pass, after the numbers above, asking whether 5-10 fps was reachable.
Two of the three paid; the third answered a different question than it was
asked.

**9. VP8 screen content mode 2.** `VP8E_SET_SCREEN_CONTENT_MODE` tells the
encoder it is looking at a desktop rather than a camera. Three heuristics in the
realtime mode picker are conditioned on it and all three are wrong here: the
dot-artifact check and the skin-map lookup are camera work, and there is a
ZEROMV rate-distortion bias tuned for natural video. Mode 2 also keeps a golden
frame updated, which is what a mostly-static screen wants. Swept against a frame
with 30% of its macroblocks active — what a scrolling xterm actually reports:

```
  profile 0 (libvpx default)       busy 560 ms    small 327 ms
  profile 3  (what we shipped)     busy 364 ms    small 193 ms
  profile 3 + screen content 1     busy 327 ms    small 291 ms
  profile 3 + screen content 2     busy 294 ms    small 215 ms
  profile 3 + last ref only        busy 357 ms    small 193 ms   (still a no-op)
  profile 3 + min_q 24             busy 296 ms    small 187 ms   (costs quality)
  profile 3 + thresh 15000         busy 278 ms    small 139 ms   (half-character bug)
```

Mode 2 is now the default on IRIX. `last_ref_only` measures nothing here either,
exactly as it measured nothing on the G5 — that result now has two machines
behind it.

**10. Above 1/2, sample the box instead of averaging it.** The conversion's cost
is dominated by *reading the source*, not by writing the output: at 1/4 each
destination pixel averages sixteen source pixels, so shrinking the output does
not shrink the work. It showed up plainly once the encoder stopped dominating —
at 1/4 on a scrolling xterm the encode had fallen to 118 ms and the conversion
was still 100-120 ms, one of the two largest items left.

Averaging a 2x2 sample of each box reads a quarter of the source at 1/4 and a
sixteenth at 1/8. Full-screen conversion at 1/4: **317 ms -> 121 ms**. The
quality argument is that at 1/4 a 1280x1024 desktop is already 320x256, text is
unreadable either way, and what the full box buys there is a smoother version of
something nobody is reading. At 1/2, where text is marginal and a box is only
four pixels anyway, the full average is kept.

**11. The skipped macroblocks' pixel copy, out of the per-macroblock loop.**
Every macroblock the active map excludes still had to have 384 bytes of
reference pixels moved into the reconstruction buffer so the frame is complete
— 5120 small scattered copies on a 1280x1024 frame. Seeding the whole
reconstruction buffer from the reference once, in
`init_encode_frame_mb_context()`, is the same bytes with the locality and none
of the per-call overhead.

Priced first with a deliberately-wrong build that simply omitted the
per-macroblock copy and produced garbage in the untouched regions. That is the
cheap way to find out whether something is worth engineering around before
engineering it:

```
                            floor at 1280x1024   at 640x512
  stock libvpx                     753 ms          166 ms
  + early exit                     395 ms          113 ms
  + wrong build, no copy at all    250 ms           77 ms   <- the price tag
  + the copy done once, correctly  259 ms           83 ms
```

**Copy the planes, not the frame.** The first attempt used
`vp8_yv12_copy_frame`, which extends the borders afterwards — slower than the
copy it follows, and redundant, because `vp8_setup_intra_recon` sets them up and
the end of the frame extends them again. It made the small sizes *worse*
(160x128 went 21 → 40 ms, on a plane copy of 30 KB). A plain row-wise `memcpy`
of the visible planes is what was wanted.

Full resolution is where this lands: an interactive frame at 1280x1024 went from
443-623 ms to **287-342 ms**, so the ceiling moved from 2.5 to 3.9 fps on the
emulator and, by the ÷3 estimate, into double figures on a real Indy.

**12. iris `jitv2` is not usable here yet.** Built upstream at `02c4e155` with
`--features lightning,rex-jit,jitv2,r5k,chd` on the theory that a CPU JIT would
make the whole measurement loop faster. It boots IRIX in about the same time as
the interpreter (290 s), gives roughly 15-20% on the encoder floor — and then
**the X server vanishes mid-session**, with no core, nothing in `/tmp/xsgi.log`,
and the emulator log churning `mega_flush ... 50624 functions compiled`.
Reverted. The build is at `~/iris-jitv2-target` if anyone wants to chase it;
worth telling the iris side that jitv2 plus Xsgi loses the server.

**Two emulators against one CHD, again.** Starting the jitv2 build did not kill
the previous emulator — the `kill -TERM` did not take, and for ten minutes two
processes had the same disk open. The base image was untouched (iris writes to
the `.diff.chd` sidecar) and the diff was discarded, but this is the second time
this trap has been walked into. The new one gives itself away in the log:
`TCP port forward 127.0.0.1:2324 failed to bind: Address already in use`.

### Where the wall is

At 320x256 on a scrolling xterm, per frame: **probe 109-205 ms, conv 100-120 ms
(now ~35), enc 113-122 ms.** The encoder is no longer the largest item, and
`probe` — the X server copying damaged pixels into the shared canvas at
*framebuffer* resolution — cannot be reduced from this side. There is no scaling
hint in the ReadDisplay extension (`XRD_READ_ALPHA`, `XRD_TRANSPARENT`,
`XRD_READ_POINTER` and the layer masks are the whole list), so a change covering
29% of a 1280x1024 screen costs 1.5 MB of copying whatever size it is served at.

So a full-window repaint is close to the floor of this design, and the remaining
option is the structural one: **tiles as displays**, which would let a small
change be a small *frame* rather than a small part of a big one.

### How fast is it, honestly

One number for "the agent" is close to meaningless: the two ends of the workload
range differ by an order of magnitude, and a remote desktop spends its life at
the cheap one. Measured with `ports/iris-run/guest/fps-matrix.sh`, 45 s each,
emulated R5000 — divide by roughly 3 for a real Indy, and an O2 is faster again:

| | 1280x1024 (Best) | 640x512 (Balanced) | 320x256 (Low) |
|---|---|---|---|
| pointer moving, 6.7 moves/s | 1.5 fps | 4.5-4.7 fps | 5.0 fps |
| a word typed per second | — | 1.6 fps | — |
| xterm repainting its whole window twice a second | 0.12 fps | 1.3 fps | 1.9 fps |
| **encoder floor (nothing active at all)** | **259 ms** | **83 ms** | **41 ms** |

**The first two rows are the rate of change, not the ceiling.** A screen that
changes six times a second reports six frames a second however fast the agent
is. The agent kept up with every single pointer move at Low, and nearly every
one at Balanced. What the ceiling actually is comes from the per-frame line:

```
small change  1280x1024:  probe 4,  conv 1,  enc 296 = 312 ms   ~3 fps
small change   640x512:   probe 3,  conv 0,  enc  96 = 106 ms   ~9 fps
small change   320x256:   probe 3,  conv 0,  enc  40 =  49 ms  ~20 fps
whole window   640x512:   probe 115, conv 154, enc 389 = 705 ms
whole window   320x256:   probe 152, conv 102, enc 113 = 378 ms
```

At 1280x1024 `probe` and `conv` have vanished entirely — 4 ms and 1 ms — and the
frame **is** the encoder walking 5100 macroblocks it has been told to ignore.
That is why items 3 and 11 above are both about libvpx and neither is about
capture.

So: **ordinary interaction is comfortably in the 5-20 fps range on the emulator
already**, and full-window repaints are not, at either size. Driving the pointer
harder than 6.7 moves a second does not raise the number — it lowers it, because
each move is an XTEST round trip on a one-processor machine and input starts
competing with the encoder (25 moves/s gave 3.1 fps).

`image_quality` from the peer, one run, three 60-second sessions back to back
against the scrolling screen (`quality-sweep.sh`):

| peer asks | served | fps | bytes/frame |
|---|---|---|---|
| Best | 1280x1024 | 0.12 | 4337 |
| Balanced | 640x512 | 0.80 | 3367 |
| Low | 320x256 | 1.43 | 1084 |

**Take the ratios, not the absolutes.** This host is shared and its load moved
between 4 and 15 across the session; the same Balanced configuration measured
0.80, 1.26, 1.30 and 1.40 fps at different moments, and the sweep above ran
during the worst of it. That is why the sweep is one run of three sessions
rather than three runs — within a run the comparison holds.

Balanced prints no "switched the display" line because 1/2 is already the
default and `set_scale` correctly declines to rebuild for a no-op.

Full resolution is now *slow* rather than impossible — before this session a
single 1280x1024 inter frame cost 10-18 seconds.

### Where the remaining time goes

At 640x512 on a busy frame: `probe` ~110-320 ms (the server's damage copy, at
framebuffer resolution), `conv` ~140-230 ms, `enc` ~280-430 ms. On a small
change the whole frame is ~100 ms and `enc` is nearly all of it.

The next lever, if this needs to go faster again, is **tiles as displays** —
`docs/performance-plan.md` option 9. It is the only remaining option that
changes the cost model rather than tuning it: a fixed grid of tiles declared as
separate displays in `PeerInfo`, each with its own encoder, only the tiles that
moved sent. It would give full-resolution text at the frame rate 640x512 gets
now. It is large, it needs N encoders, and it depends on client behaviour that
must be read in the client's source first.

---

## SELF-HOSTED INFRASTRUCTURE — the CLI was complete, two paths were not

Everything a self-hosted RustDesk deployment needs is already in the agent's
command line, and every one of them is persisted to the config file:

```
--server HOST[:PORT]   rendezvous (hbbs), default port 21116  -> rendezvous_server
--relay-server HOST    relay (hbbr) override; empty means whichever the server names
--key KEY              the server key, the same string a client puts in its Key field
--api-server URL       the console, so the machine appears in its device list
--ca-bundle PATH       certificates for a console behind a private CA
--no-server / --no-api-server    stop doing either
```

`--server` makes the machine *reachable* by ID; `--api-server` makes it
*visible* in a console. They are independent and neither implies the other.

**Neither had ever been run from the IRIX build, and both were broken — by the
same fault, in two more places.** IRIX has no `SO_RCVTIMEO` or `SO_SNDTIMEO`;
`set_read_timeout` returns ENOPROTOOPT (99). `session.rs` already knew that. Two
other places did not:

- **`http.rs` took it as fatal**, so every HTTPS request failed before the
  handshake started. That is the whole of `--api-server`: the console heartbeat
  is an HTTPS POST, so the console could never have worked. Now logged and
  continued past; `CONNECT_TIMEOUT` still bounds getting there.
- **`rendezvous.rs` took it as fatal too**, and worse: that loop is *built* on
  the UDP read timing out, because that is what tells it to resend. Dropping the
  timeout would have replaced a dead loop with a wedged one, so the wait moved
  to `poll(2)` — `sys::wait_readable_fd`, the `AsRawFd` sibling of the
  `TcpStream` helper the video loop already uses.

Both verified on the target against stand-ins on the build host, so the check
does not need anybody's real infrastructure:

```
--- tls (real handshake to the build host) ---
  [PASS] CaBundle::load          CaBundle(/tmp/testca.pem, 1144 bytes)
  [PASS] Url::parse              host=192.168.0.1 port=8443 secure=true
  [PASS] https POST /api/heartbeat   status 200, 18 bytes: {"modified_at": 0}

[  0.919] INFO  rendezvous: registering with 192.168.0.1:21116 as id ff6izj02b
[  1.006] INFO  rendezvous: registered, reachable as id ff6izj02b
```

and confirmed from the far end — the HTTPS server logged the exact JSON body,
and the UDP stand-in logged the `RegisterPeer` datagrams arriving fourteen
seconds apart, which is the refresh interval.

**This is the first TLS handshake this port has ever done.** mbedTLS passing its
own selftest and a plain TCP GET working are neither of them the same claim, and
the gap between them hid a one-line bug for the whole port.

`ports/iris-run/tls-endpoint.sh` runs the HTTPS stand-in on the host (and writes
the `testca.pem` the guest fetches); `guest/rendezvous-check.sh` drives the
registration loop. The UDP stand-in is small enough to be worth keeping in the
session notes rather than the repo.

### THERE IS A GUI ON IRIX NOW

`gui/gui_motif.c`, built as `rustdesk-agent-gui` by `gui/build-gui.sh`. A Motif
settings panel with the two things a person needs — what this machine is, and
what deployment it should join — plus Start/Stop/Restart/Refresh and a status
line. Seen running on the emulated Indy under 4Dwm; `RESUME-PROMPT-GUI.md` has
the screen and the detail.

**No behaviour in the window.** Every button runs one verb of
`gui/agent-helper.sh`, which is ordinary Bourne shell and testable from a
terminal, and each `set` verb is exactly one `rustdesk-agent --flag` invocation
— so the panel cannot drift into having its own idea of what a setting means,
and anything it does can be done without it. The split, and most of the idiom,
is taken from `../irixscsitb`'s `gui_motif.c`: Motif 1.2 only, the
`useSchemes`/`SgiSpec` fallback resources for the SGI look, every label a
resource rather than a string literal, the `XtArgcType` typedef for the R4/R6
argc difference, an XBM window icon because 5.3 ships `libXpm.so` with no
header, a busy cursor pushed out with `XmUpdateDisplay`, and reusable error and
info dialogs with Cancel and Help unmanaged.

**Motif cross-compiles, which was not a given.** Two things the stock
environment does not provide, both handled in `build-gui.sh` and both worth
knowing before reaching for any other IRIX toolkit:

- **The Motif headers are not in `/opt/irix-sysroot`; the libraries are.** They
  are `libXm.so`, `libXt.so`, `libSgm.so` — all *shared*, which is the only
  reason this works at all, since LLD cannot read the static archives SGI ships
  (§LLD cannot link SGI's static archives). On the machine `/usr/include/Xm` is
  a **symlink** to `/usr/Motif-1.2/include/Xm`, so `tar cf` of `/usr/include/Xm`
  stores a link and nothing else — a 10 KB tarball that looks like it worked.
  Tar the real directory. Now installed in the sysroot: Motif **1.2.4**.
- **`-D_XmConst=`.** `Xm/XmStrDefs.h` declares `externalref _XmConst char ...`
  for SGI's keypad virtual keys inside a branch that never defines `_XmConst`.
  MIPSpro tolerates it, clang stops with "unknown type name". Defining it empty
  on the command line is the entire fix and changes no generated code.

**The layout is XmRowColumn and that is the third attempt.** `XmForm` with edge
attachments collapsed the panel to a 180-pixel stub — children attached to both
edges of a Form is a circular size negotiation and Motif resolves it by
collapsing. Forcing a shell geometry afterwards made it worse: correctly sized,
completely empty. And `XmFrame` around each group, with or without its own title
child, gave its work area one row less height than it asked for, so **the last
row of every group was silently clipped** — four rows where there should be
five, a panel that looked entirely right, and nothing in any log. A spacer at
the end absorbed it in a four-row group and not in a five-row one, so it is not
a fixed number of pixels. Groups are now a heading and a rule, no frame.

**Pressed, as of 2026-08-24** — see §THE BUTTONS WORK, which is where the
detail is. Start, Stop and one Apply have been clicked by injected input and do
what they say, and typing into a text field works. Three bugs came out of it,
all of which had been sitting in code that nothing had ever exercised.

### THE BUTTONS WORK

Pressed, on 2026-08-24, by injected input — not by a person at the machine and
not inferred from the code:

```
=== 1. click the relay field, type, click Apply ===
relay_server before: []
click 319,313
type "relay.press.test"
click 482,313
relay_server after : [relay.press.test]

=== 2. click Start ===   agents running: 1
=== 3. click Stop ===    agents running: 0
```

That single run closes three open items at once:

- **A click reaches a Motif widget** and lands where it is aimed: into a
  TextField to place the caret, onto a PushButton to fire its callback.
- **`rd_key_char` types into a focused window.** Sixteen characters, dots
  included, arriving exactly. This had never happened — the bare X server had
  nothing focusable on it until the panel existed, which is why §Next steps
  carried "keyboard injection is written but unproven" for two sessions.
- **`apply_cb` runs the helper and the setting reaches the config**, so the
  whole path from pointer to `rustdesk-agent --relay-server` to
  `~/.rustdesk-ppc-agent.conf` is proven end to end.

`ports/iris-run/guest/gui-press.sh` is the run, and it leaves the machine as it
found it.

**The coordinates are not read off a screenshot.** `gui_motif.c` prints every
managed widget's position in root coordinates when `RD_GUI_GEOM` is set — a
timeout after realize, because the window manager places the shell afterwards
and `XtTranslateCoords` before that reports where the shell *asked* to be. The
capture is at 1/2 scale, so a button read off one is ±2 native pixels before any
arithmetic, and a click one pixel outside a PushButton does nothing at all and
looks exactly like injection being broken.

`probes/xpoke.c` is the clicker: the agent's own `input_shim.c` with a command
line in front of it, so `xpoke click X Y type TEXT click X Y` is one process and
one X connection. Nothing about it is agent-specific and deleting it changes the
agent not at all.

#### Five bugs it found, all of which looked like something else

**`agent-helper.sh` could not see a running agent.** `ps -e` truncates COMD to
eight characters on IRIX, so `ps -e | grep rustdesk-agent` matches **nothing**.
`is_running` therefore always answered "no": Stop killed nothing and still said
"Stopped.", Start said "Could not start it" about an agent it had just started,
and the panel's Status line said "stopped" for ever. Every one of those looks
like a broken button. The first fix was `ps -e -o pid,args` plus a `grep -v
rustdesk-agent-gui` — without the second half Stop takes down the window that
pressed it. That fix was itself wrong in a way only an installed machine shows;
see the last bug in this list.

This is the eight-character trap that `gui-shot.sh` already had a comment about.
It was in the helper the whole time and nothing had exercised the path.

**The panel would not have found its helper once installed.** `find_helper`
looked beside `argv[0]`, then in a SGUG path, then `/tmp`. Installed, the panel
is `/usr/sbin/rustdesk-agent-gui`, so "beside argv[0]" is `/usr/sbin` and finds
nothing — and on the development guest it fell through to the `/tmp` copy and
looked fine. On anyone else's machine it would have found no helper at all. The
installed path is now first. The same fault, and the same fix, for the helper's
own idea of where the agent is: it defaulted to a SGUG path and fell back to
`/tmp`, so after a successful install it reported `agent=/tmp/rustdesk-agent`.

**A multi-line status message draws outside the window.**
`XmStringCreateLtoR` turns every newline into a line of the label, a Label
recomputes its size from its string, and with no window manager to renegotiate
the shell the label simply paints past the bottom of the window onto the root.
It looks like the panel has corrupted the screen. `set_status` now flattens and
bounds the text, and the label carries `XmNrecomputeSize False`.

**The status line was reporting the wrong thing entirely, and that is what put
a blob of `key=value` in it.** `helper()` returns a static buffer, and both
`apply_cb` and `lifecycle_cb` did

```c
out = helper(verb, NULL, NULL);
refresh_state();          /* calls helper("status") -- overwrites the buffer */
set_status(out);          /* so this prints the STATUS BLOB, never the reply */
```

so the line after every button press was `agent=/usr/sbin/rustdesk-agent
present=yes running=yes id=...` rather than "Started." or "Relay set to ...".
It had been that way since the panel was written and nobody had pressed a
button to see it. Both callbacks now copy the reply before refreshing.

**`is_running` matched its own shell, but only once installed.** The fix for
the eight-character truncation was `ps -e -o pid,args | grep rustdesk-agent`,
which matches too much: the panel (`rustdesk-agent-gui`) — handled with a
`grep -v` — and, after installation, **the helper's own shell**, because the
helper lives in `/usr/lib/`**`rustdesk-agent`**`/agent-helper.sh`. So on an
installed machine `is_running` always answered yes: **Start was greyed out for
ever**, and Stop's kill loop would have killed the helper mid-run.

This is the one that justifies `scripts/iris-install-test.sh` existing. It
cannot happen in `/tmp` and it cannot be seen on the build host; it appears the
moment the software is in the place it is meant to live, and it makes the
principal button permanently dead. The matcher is now the basename of `argv[0]`
compared exactly — a process is the agent if it *is* the agent, not if its
command line mentions it — and the same three-line awk is in `install.sh` and
`gui-press.sh`, both of which had the same exposure.

#### And again, against the INSTALLED panel

Everything above was the `/tmp` build. The same script run as

```sh
RD_GUI=/usr/sbin/rustdesk-agent-gui RD_HELPER= sh /tmp/gui-press.sh
```

drives the panel the package installed, with `RD_HELPER` empty so the panel has
to *find* its helper the way a person double-clicking the Toolchest entry would:

```
panel: /usr/sbin/rustdesk-agent-gui, helper: /usr/lib/rustdesk-agent/agent-helper.sh
relay_server before: []      ->  after: [relay.press.test]
click Start   agents running: 1  (after 0s)
click Stop    agents running: 0  (after 4s)
```

`docs/panel.png` is what it looks like at the end of that: status line reading
"Stopped.", Start live, Stop greyed. Both of the installed-only bugs above were
found by exactly this run and neither was visible any other way.

#### What still has not been pressed

Restart, Refresh, the menu items, and the five Apply buttons other than Relay.
The mechanism is identical for all of them — `apply_cb` with different client
data — so the risk is low, but the distinction between "proven" and "the same
code with a different argument" is the whole reason this section exists.

---

### THE PACKAGE

`scripts/release.sh` produces a `.tardist` that installs with `inst`(1M) and a
`.tar.gz` with an `install.sh` in it. Verified by installing it in the guest and
running what came out:

```
I  rustdesk_agent       2026082454  RustDesk agent for IRIX
I  rustdesk_agent.sw.n32            RustDesk agent, settings panel and helper

/usr/sbin/rustdesk-agent --show-id   ->  ff6izj02b
```

**with `LD_LIBRARYN32_PATH` unset**, which is the point. `libgcc_s.so.1` is the
one library the agent needs that stock IRIX 6.5 does not ship — everything else
it links is on the machine, and libsodium, libvpx, mbedTLS and zstd are static.
The binary is linked `-Wl,-rpath,/usr/lib/rustdesk-agent` and the package puts a
copy there, so there is no environment variable and no wrapper. Every other
script in this tree sets `LD_LIBRARYN32_PATH`, so an rpath that quietly did
nothing would never have shown up; `scripts/iris-install-test.sh` unsets it on
purpose.

The other half of the release installs by hand:
`scripts/iris-install-test.sh --tarball` unpacks the `.tar.gz` in the guest,
runs `install.sh -p /opt/rdtest`, runs the wrapper it wrote **with no
environment set** (`ff6izj02b`), and takes it all away again with `-u`. That
path exists for a machine without swmgr and for a site that wants the files
somewhere else.

**The install test is not part of a release** — decided 2026-08-24. It roughly
doubles a run, ten minutes of twenty and nearly all of it inside `inst` on an
emulated R5000, and shipping a package should not cost that every time. Opt-in
everywhere: `release.sh --install-test`, or the workflow's `install_test`
input, both off. Run it when the **packaging** changes — the idb, an install
path, what the helper or the panel looks for — which is the only kind of change
it has ever caught anything on. The trade is deliberate and worth naming: a
quick release, and a class of install-only bug (§THE BUTTONS WORK has the one
it caught) that will get through until somebody runs it.

`docs/PACKAGING.md` is the whole pipeline. The shape is `../irixscsitb`'s: an
`inst/` product description whose version and ABI are stamped in at build time,
a `desktop/` Toolchest fragment, and scripts thin enough that the CI workflow
would be one line per step — `ci/workflows/irix-agent-release.yaml` is that
workflow, kept out of `.github/` because this tree lives inside a fork of
upstream RustDesk and would otherwise fight upstream's own CI. The one step
that cannot happen on Linux is `gendist`, which only exists on IRIX and runs in
the guest.

**Everything that talks to the guest now goes over the serial console.** See
§Operating the emulator; the telnet forward stalls and this pipeline must not
need a person.

#### And it brings its own guest

`--boot` is what makes the emulator steps runnable by something other than the
person who set this machine up:

```sh
scripts/release.sh --boot --install-test    # cold: nothing running, no one watching
scripts/release.sh                          # warm: attaches to a running guest
```

Three scripts, and each one closes a specific hole:

- `scripts/fetch-image.sh` resolves the boot image: `$IRIX65_IMAGE`, then
  `ci/local.conf`, then `$IRIX65_DISK_URL` — a private download URL, which is a
  repository secret in Actions. **Those are ../irixscsitb's variable names on
  purpose**: the same licensed image and the same secret serve both repos.
- `scripts/fetch-iris.sh` resolves the emulator: a local build wins, otherwise
  the prebuilt CLI from an iris release. Building iris from source is a Rust
  toolchain plus clang and libclang for the chd feature.
- `scripts/iris-guest.sh` boots one and disposes of it. **It never writes to
  the image** — `overlay = true`, sidecar deleted on stop — and it takes **no
  port forwards**, because the pipeline needs none: commands over the console,
  files in over HTTP (the guest dialling out, not a forward), files out through
  `iris-ci get`. No host ports means two runs cannot collide and neither can
  steal the telnet port from a developer's own emulator. The control socket is
  per-run for the same reason: iris *deletes and rebinds* whatever socket path
  it is given, which is how a session once stole another session's.

`release.sh --boot` starts **one** guest and both emulator steps share it. A
boot costs a couple of minutes here, so one per step would have doubled a run
for nothing.

**Readiness is "a command runs", not "a banner appeared."** `iris-ci boot` is
the obvious call and does not work on this image: the PROM autoboots straight
past the menu it watches for, so it sat through its whole 600 s timeout while
`IRIS console login:` had been in `console.log` for three minutes. This file has
warned since the first session that the serial banner is a bad readiness test
here — telnetd answers well before it prints — and this is that same fact from
the other side. `iris-guest.sh` polls `iris-ci run 'echo GUEST-READY'`.

**And it is five minutes a run faster.** The guest answered a command 110 s
after `iris-ci start`; the banner did not appear for another five minutes. The
note about telnetd was right about the console shell too, and every run that
waited for the banner was throwing that away.

Two more things that run only bought:

- **`eval "$(cmd)"` hides a failure from `set -e`.** `eval` returns the status
  of the string it evaluated, and a dead command evaluates to the empty string,
  which is success. So a guest that never came up produced a release run that
  carried cheerfully on to "no iris on /tmp/iris-rdagent.sock" — a confusing
  error about the wrong socket, three steps after the real one. Capture, check,
  then eval.
- **Set the cleanup trap BEFORE starting anything**, not after. The version
  that set it afterwards would have leaked an emulator for exactly the failure
  it was written to handle.

- **A full host disk panics the guest, ten minutes in, with no clue why.** The
  guest writes everything to a copy-on-write overlay on the build host. When
  that host filled to 100% underneath a packaging run, the write failed and
  IRIX treated it as fatal:

  ```
  ALERT: I/O error in filesystem ("/") meta-data dev 0x38 ... ("xlog_iodone")
  PANIC: |$(3726)Fatal error on root filesystem
  ```

  followed by a dump that also fails, and `[Press reset to restart the
  machine.]`. Nothing in that names the actual cause. `iris-guest.sh` now
  checks `df` before it boots anything — 2 GB floor, `IRIS_GUEST_MIN_MB` to
  override — and watches `console.log` for `PANIC:` while it waits, because a
  panic is indistinguishable from a slow boot to a poll that only asks whether
  a command ran.

- **`iris-ci get` can pick the wrong shell.** It probes `echo ZZSHELLZZ=$0` and
  chooses sh or csh syntax from the answer; on a console that has just been
  worked hard the probe can miss, and it then sends `>& /dev/null` and
  `$status` to bash. The transfer fails reporting "iris-ci get needs a shell on
  the serial console" — and the shell is right there. `guest_get` in
  `scripts/ci-lib.sh` settles the console and retries three times; one retry has
  always been enough. Worth reporting upstream, with the detail that should
  narrow it: **it has failed on the SECOND `get` of a run both times it has
  happened**, never the first or the third. `get` has no `--shell` flag the way
  `run` does, which would remove the guesswork entirely.

- **`iris-ci login` fails when the console is ALREADY logged in.** It types
  `root` at a shell, gets "root: command not found", and reports that it never
  saw the sequence it was looking for. Treating that as an error made the
  install test refuse to start on a guest that had just run gendist perfectly
  well — "could not get a shell on the serial console", about a console with a
  shell on it. `guest_login` now asks whether a command runs and only logs in
  when one does not. That is the third time in one session that the same lesson
  arrived wearing a different hat: **ask whether the thing works, not whether a
  proxy for it looks right.** The X wedge, the boot banner and this are all the
  same mistake.

And one worth remembering separately, because it is easy to do by reflex:
**editing a shell script that is currently executing corrupts the run.** `sh`
reads a script incrementally, so a patch applied while one is mid-flight moves
the ground under the byte offset it is reading from. It happened here to
`iris-guest.sh` during its own cleanup path. Wait for the run, or copy the
script and edit the copy.

---

### The Mac's GUI, and why the IRIX one was cheap

The Mac has one: `deploy/app-ui.m`, a Cocoa settings window with exactly these
fields — password, ID server, relay, key, console, CA bundle — plus install,
start, stop and restart. It is deliberately split: **everything that decides
anything lives in `deploy/agent-helper.sh`**, and the window only calls into it.
Its own header says why — the interface is the part that can only be exercised
by a person at the machine, so it should hold as little behaviour as possible.

That split is what makes an IRIX version small: the helper is portable Bourne
shell, and only the window would have to be written. The guest has **Motif
shared** (`/usr/lib32/libXm.so`, and SGI's `libSgm.so` for the Indigo Magic look
and feel), which our toolchain can link — unlike `libXtst`, which SGI ships only
as a static archive LLD refuses. So a native settings panel that matches the
desktop is a contained piece of work rather than a port of anything.

Which is what made the IRIX panel a day rather than a week: the helper was
written from scratch but the *shape* was already decided, and every hard
question about Motif on this platform had been answered next door.

---

## A REAL LOGIN SESSION — the authority answer is good, the emulator is not

Every measurement in this file was taken against a bare `Xsgi :0 -bs -c` started
by `/root/restart-x.sh`, which has **no access control at all**. That is not how
a real machine runs, and the obvious worry was authority: xdm normally starts
the server with `-auth` and a MIT-MAGIC-COOKIE that lives in xdm's authdir and
is copied into the *logged-in user's* `~/.Xauthority`. An agent running outside
any session would get `No protocol specified` and go blind the moment somebody
logged in — and blind at the login screen too, which is where remote access
matters most.

**Tested, and the answer is that it does not arise on this image.** xdm here
starts the server as

```
/usr/bin/X11/Xsgi -bs -nobitscale -c -pseudomap 4sight -solidroot sgilightblue
```

with **no `-auth`**. Access is host-based through `/etc/X0.hosts`, which already
contains `IRIS` and `localhost`. X access control is a disjunction — an allowed
host *or* a valid cookie — so the agent needs no cookie. Keep `/etc/X0.hosts`;
it turned out to be irrelevant to the bare server and it is the whole mechanism
under xdm.

**What could not be tested is everything after that, because the xdm-started
server wedges under IRIS.** Same signature as §THE BLOCKER, RESOLVED, and it is
*not* our client and *not* authority:

```
Xsgi 5558   0:02   ... -solidroot sgilightblue      at T
Xsgi 5558   0:02   ... -solidroot sgilightblue      at T+30s   CPU time frozen
DISPLAY=:0 xdpyinfo  -> killed after 25 s                      IRIX's own client hangs
```

No `ng1 pixel dma` warning on the console this time, and `iris-ci screenshot`
came back all black. Upstream iris `02c4e155` ("fix hostr readback issues")
cured the wedge our *capture load* used to provoke; this is a second one, which
xdm's visual-login rendering still provokes. **Worth reporting to the iris side
as a distinct case** — `docs/ISSUE-xdm-wedge.md` is written and ready to send,
`ports/iris-run/guest/xdm-check.sh` reproduces it in one run, and `xdm-stop.sh`
puts the bare server back.

So three things stay unproven and all three need real hardware or a fixed
emulator:

- **The agent surviving the server restart xdm does between logins.** This is
  already the open item in §The agent can crash when the server dies underneath
  it, and under xdm it stops being an edge case: it happens at every logout.
- ~~**Keyboard injection with something focused.**~~ Done, and it did not need
  xdm after all: the settings panel is itself something focusable, and typing
  into its Relay field works (§THE BUTTONS WORK). What a login session would
  still add is typing into somebody else's application rather than ours.
- **Damage volume on a real 4Dwm desktop.** Everything measured here is one
  xterm and a clock. Watch the `N/1280 MB` figure in the frame line: if it sits
  near the total, the rectangles are being merged and `MAX_RECTS` wants raising
  again.

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
                       session.rs needs no changes, adds poll_rects() for the
                       finer answer, and to_i420_rect() -- the frame loop's hot
                       path, which fuses the downscale with the A,B,G,R -> I420
                       conversion over a destination rectangle.
probes/rawx.c          hand-rolled X11 setup, no Xlib — the probe that split the blocker
probes/xcapture.c      format and cost probe (byte order, XRD_READ_POINTER)
probes/xshmcap.c       shm + XShmReadDisplayRects: geometry, cost by shape, colormap
probes/sgicap.c        SGI-SCREEN-CAPTURE end to end
probes/capture_test.c  exercises capture_shim.c; modes: all | 0 | 1 | 2 | reopen
probes/reopen.c        the XCloseDisplay experiment, one variant per process
probes/xpoke.c         aim input_shim.c at a coordinate from a command line:
                       `xpoke click X Y type TEXT click X Y` in ONE process, so
                       driving the panel costs one X connection rather than one
                       per action. Deleting it changes the agent not at all.
gui/gui_motif.c        the settings panel: the window, and nothing that decides
                       anything. RD_GUI_GEOM makes it print every widget's root
                       coordinates, which is how it gets driven.
gui/agent-helper.sh    everything that decides anything, in Bourne shell, one
                       `rustdesk-agent --flag` per setting
gui/build-gui.sh       cross-build the panel. Motif 1.2.4, -D_XmConst=
inst/                  the inst(1M) product description; version and ABI stamped
                       in at build time by stage_inst_inputs
desktop/RustDesk.chest the Toolchest entry, a drop-in fragment that edits no
                       system file and hides itself when the program is gone
scripts/               the pipeline. build.sh -> iris-gendist.sh -> package.sh,
                       with release.sh running all three and
                       iris-install-test.sh proving the result installs.
                       ci-lib.sh is the shared half, including guest_run(),
                       which is how anything automated talks to the guest.
ci/workflows/          a GitHub Actions workflow, kept OUT of .github because
                       this tree lives inside a fork of upstream RustDesk
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
                         perfprobe [modes]  where a frame's milliseconds go, one
                                            stage and one FRAME at a time. Modes:
                                              bytes    the memory byte order, settled
                                              convert  fused against two-pass
                                              encode   keyframe against inter
                                              amap     the active map's saving
                                              dump     write a PPM of the converted screen
                                              floor    the fixed cost of a frame, by scale
                                              sweep    encoder settings against a real frame
                                              verify   the peer's picture against the screen
                                              loop     a damage-driven steady state
                                            No argument runs everything but `verify`.
ports/iris-run/gsh.py  marker-based telnet runner (see below)
ports/iris-run/serve.sh
                       serves the build and the guest scripts on :8099, which is
                       where the guest's wget fetches from. Copies guest/*.sh
                       into the build directory first -- they live in git
                       because target/ is an artifact and losing the harness
                       with it costs an afternoon.
ports/iris-run/guest/  the guest-side harness, all fetched by fetch.sh:
                         fetch.sh          pull the current build onto the guest
                         run-agent.sh      restart the agent, listening, with -v
                         setup-screen.sh   xclock + a scrolling xterm, so the
                                           damage path has work to do. A blank
                                           root looks exactly like a broken agent.
                         quality-sweep.sh  the image_quality dial, one run
                         decode-check.sh   three coloured xterms -> decode -> pixels
                         colour-check.sh   the same without a session
                         fidelity-check.sh does the peer's picture track the screen
                         mouse-check.sh    the coordinate scaling, against a live agent
                         ab-floor.sh       the libvpx patch, interleaved A/B
                         xdm-check.sh      can the agent capture an xdm-owned display
                         xdm-stop.sh       put the bare server back afterwards
                         cleanpty.sh       reap stale telnet logins (see gsh.py)
patches/               libvpx-vp8-active-map-early-out.patch, and why it exists
```

### Building for the target

```
./ports/rust/build-compat.sh     # once per clone; see below
cd ports/rust/agent-portable
. ../env.sh                      # private RUSTUP_HOME/CARGO_HOME + SODIUM_LIB_DIR etc.
cargo +nightly build --release
```

`build-compat.sh` builds `librust_irix_compat.a` from mogrix's compat sources.
It is a build artifact, so it is not in git — and `agent-portable/compat` is a
symlink to `hello/compat`, which means **a fresh clone has a dangling symlink
until you run it**. Skipping it fails the link on an unresolved
`-lrust_irix_compat`, which names the archive but not the reason.

If the link fails on an unresolved `-lrust_irix_compat`, `env.sh` was not
sourced.

### Prerequisites: the C libraries the agent links

**These are not reproducible from any repo, and that is a gap rather than a
decision.** All four were built by hand into `/opt/sgug-staging/usr/sgug`, which
is outside every git tree here, so a fresh clone links against whatever happens
to be on the machine — or fails with `unable to find library -lzstd` and no
indication why. The invocations below are recovered from the build trees'
`config.status` and `config.log`, so they are what was actually run.

Sources live in `ports/work/`, which is gitignored (third-party trees and
tarballs, not ours to vendor).

```sh
CC=/opt/sgug-staging/usr/sgug/bin/irix-cc
PREFIX=/opt/sgug-staging/usr/sgug

# libsodium 1.0.18 -- no patches
./configure --host=mips-sgi-irix6.5 --prefix=$PREFIX \
    --disable-shared --enable-static --disable-pie --disable-ssp \
    CC=$CC AR=/opt/cross/bin/llvm-ar RANLIB=/opt/cross/bin/llvm-ranlib CFLAGS=-O2

# libvpx 1.13.1 -- one patch (see mogrix rules/packages/libvpx.yaml)
./configure --target=generic-gnu --prefix=$PREFIX \
    --disable-shared --enable-static \
    --enable-vp8-encoder --enable-vp8-decoder \
    --disable-vp9-encoder --disable-vp9-decoder \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests \
    --disable-runtime-cpu-detect --disable-webm-io --disable-libyuv

# mbedTLS 3.6.2 -- two patches (see mogrix rules/packages/mbedtls.yaml).
# make install puts MIPS objects where a host linker may find them; see the
# note on "relocations in generic ELF (EM: 8)" below.

# zstd 1.5.6 -- no patches
cd lib && make libzstd.a CC=$CC AR=ar RANLIB=ranlib \
    CFLAGS="-O2 -DZSTD_MULTITHREAD=0"
cp libzstd.a $PREFIX/lib32/ && cp zstd.h zstd_errors.h $PREFIX/include/
```

**zstd should not have been built by hand at all.** mogrix already ships
`rules/packages/zstd.yaml`, which predates this work. It is correct: all nine of
its `spec_replacements` match the f40 spec, `mogrix convert` renders a proper
IRIX spec from it, and the spec ships `libzstd.a` in a `libzstd-static`
subpackage — exactly what `clipboard.rs` links. Building it by hand bypassed a
working rule for no reason.

### The mogrix pipeline cannot run on this host

Which is why everything in staging is hand-built. `mogrix setup-cross` clearly
ran here (it left `/opt/sgug-staging/rpmmacros.irix`), but the package pipeline —
`fetch` → `convert` → `build --cross` → `stage` — never has: there is no
`~/mogrix_inputs` or `~/mogrix_outputs`, and no RPM was ever produced. It cannot
run, for two environmental reasons:

- **No rpm tooling.** `rpmbuild`, `rpm`, `rpm2cpio` and `dnf` are all absent on
  this Ubuntu 24.04 host, and there is no passwordless sudo to install them.
- **Docker is installed but not usable** by this user: `permission denied` on
  `/var/run/docker.sock`, so a Fedora container is not a way round it either.

What *does* work without rpmbuild is the rule engine itself — `mogrix analyze`
and `mogrix convert` run fine and render the converted spec. That is enough to
check a rule applies, and it is how the rule bugs below were found. It is not
enough to prove anything builds.

To actually build: give this user docker group membership, or install
`rpm`/`rpmbuild`, or run the pipeline on a machine that already has them.

### The rules were describing hand builds, not the specs mogrix converts

Found by rendering each rule against the f40 spec it would be applied to:

| Rule | Status |
|---|---|
| `zstd` (upstream's) | correct, all 9 patterns match |
| `libsodium` | correct — all 4 `configure_flags` reach the converted spec |
| `libvpx` | **was broken**, now fixed |
| `mbedtls` | **was broken**, now fixed (rendering only) |

`make_target` is not a key the mogrix engine implements — it appears in no
Python file. Both `libvpx.yaml` and `mbedtls.yaml` used it for the one point
each rule says matters most, and it was silently ignored.

libvpx additionally used `configure_flags`, which hooks `%configure`; Fedora's
libvpx spec calls `./configure` directly, so none of its 14 flags reached the
build. Both are now `spec_replacements`, verified by rendering.

mbedTLS's rule described the tarball's Makefile (`SHARED=`, `make lib`) while
Fedora builds it with cmake, so none of it applied. Now expressed as cmake
options.

**Rendering is verified; nothing has been built.** Treat those two rules as
reviewed, not tested, until someone runs the pipeline on a host that can.

`clipboard.rs` is what pulls zstd in, via `#[link(name = "zstd")]`.

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

**Rebuilt from nothing on 2026-09-17, and the recipe in BUILD.md is accurate.**
`ports/rust/{rustup,cargo}` are build artifacts and were lost; only the tracked
sources survived. Recovering them took four steps and about two minutes of
work: copy `~/.rustup`'s nightly into the private `RUSTUP_HOME` (1.6 GB),
`patch-rust-sysroot.sh`, `cargo fetch` then `mogrix patch-crates`, and
`build-compat.sh` for `librust_irix_compat.a` (a fresh tree has a dangling
`agent-portable/compat` symlink until you run it). Two things made it painless
and neither is guaranteed next time: `~/.rustup` still held the exact tested
nightly, `1.99.0-nightly (1ed2df61a 2026-08-04)`, and every one of the sysroot
patcher's patterns still matched — no "already patched or no match". A
`rustup update` between sessions would have cost both.

**The shared homes must be protected while doing it.** Both patchers rewrite
state in place and neither is namespaced, so check `RUSTUP_HOME` and
`CARGO_HOME` resolve inside `ports/rust/` *before* running either. Fingerprint
`~/.rustup`'s std and `~/.cargo`'s registry first and compare after; both were
verified byte-identical afterwards, which is the only way to know the o32
effort was not damaged.

**One compile error, and it was the shared-tree drift.** `rustdesk-ppc-agent`
had gained a `libc::chown(c.as_ptr(), uid, gid)` in `config.rs` from the
SPARC/Solaris work. `std`'s `MetadataExt::uid()` is `u32` on every platform, but
IRIX's `uid_t`/`gid_t` are **signed** — the same fact item 2 above records for
std's `UserId`/`GroupId`. Fixed portably at the call site with
`uid as libc::uid_t, gid as libc::gid_t`, a no-op cast everywhere else the
shared tree builds. This is exactly the breakage `RESUME-PROMPT-PIPELINE.md`
predicted would come in through the shared tree, and nothing but compiling for
IRIX would have found it.

Two loose ends worth knowing: `std::env::consts::OS` comes back **empty** on this
target, so anything switching on it will misbehave; and the release binaries
carry one `R_MIPS_REL32` relocation, which runs fine despite `irix-ld`'s
`fix-anon-relocs failed` warning (the missing `cross/lib/elf_utils.py`).

---

## Next steps, in the order they pay

Items 1 to 4 of the previous list are **done** — see §PERFORMANCE. What is left:

0. **Point it at Dani's own deployment.** Everything on this side is done and
   verified on IRIX — `--server` registers over UDP, `--api-server` does a real
   TLS handshake, the panel writes both, and the package installs. What is
   missing is three values that only Dani has: the **hbbs hostname**, the
   **server key**, and the **console URL** with its CA if it is private. With
   those it is three Applies in the panel, or three `agent-helper.sh set` calls.
   **This is now the only thing between the O2 and a real session** — it is
   installed there (id `vjlvjqcv5`) and has never had a peer connect.

1. **Half done on hardware.** A *logged-in* desktop captures fine on the O2 —
   xdm, 4Dwm, toolchest, `xdpyinfo` answering throughout, `--probe-display`
   against the live screen. **Logging out still wedges the server**, on real
   hardware and not only under IRIS, so the greeter and the login transition
   remain uncaptured and `docs/ISSUE-xdm-wedge.md` describes something real.
   Still open besides that: keyboard injection into somebody else's
   application, and damage volume on a desktop actually in use.

2. **The agent must survive its X server dying.** Already the open item in §The
   agent can crash when the server dies underneath it, and under xdm it stops
   being an edge case: it happens at every logout. The retry path exists and the
   two known leaks are fixed, but it has never been exercised against a real
   restart. `rd_capture_close` deliberately never calls `XCloseDisplay`, so each
   reconnect leaks a `Display` — a handful over a session's life is the design,
   a reconnect storm is not. Measure RSS across retries before assuming.

3. **A packaging run is twenty minutes and ten of them are `inst`.** The boot
   was seven of them until the readiness test was fixed; now it is about two.
   If a release ever needs to be quicker, `inst` is where the time is —
   `iris-ci save`/`restore` (~145 ms) would let a run start from a snapshot
   taken after the install rather than repeating it. It needs an invalidation
   rule first: a snapshot is tied to an image *and* an emulator build.

4. **Tiles as displays**, if it needs to go faster again. The only remaining
   option that changes the cost model rather than tuning it, and the only route
   to full-resolution text at the frame rate 640x512 gets now. See the end of
   §PERFORMANCE and `docs/performance-plan.md` option 9. Large, needs N
   encoders, and depends on client behaviour that must be read in the client's
   source first.

5. **~~Keyboard injection is written but unproven.~~ Done.** `rd_key_char` has
   now typed sixteen characters into the panel's Relay field and every one
   arrived; see §THE BUTTONS WORK. `rd_key` with a Mac keycode — the
   ControlKey path rather than the `chr` path — is still unexercised, and so
   are modifier combinations: `--probe-keys X Y` sends a ctrl-C and nothing has
   yet confirmed one arrives.

6. **The clipboard and cursor paths are compiled but untested on IRIX.**
   `cursor.rs` matters less than it did — `cursor_embedded` is honoured, so the
   agent should never need to send a shape — but the code that decides that has
   not been exercised against a peer.

7. **`custom_image_quality` is still ignored.** `image_quality` is now wired to
   the picture size (§PERFORMANCE); upstream's `custom_image_quality` is a
   bitrate percentage and would belong on `bitrate_for`.

8. **The package installs but has never been through an upgrade.** A second
   `.tardist` over the top of a first one should be `replaces self` doing its
   job, and `versions remove rustdesk_agent` should leave nothing behind.
   `scripts/iris-install-test.sh --remove` runs the removal half; the upgrade
   half wants two builds with different versions and has not been done.

9. **No o32 flavor.** The scripts take `--abi` and `stage_inst_inputs` knows
   about o32 because irixscsitb's arrangement — each OS packages its own build,
   in its own guest — is worth keeping. Nothing has been built for it, and
   nothing should be until §5.3 stops being deferred.

10. **The libvpx patch has not been through the mogrix pipeline.** It is listed in
   `rules/packages/libvpx.yaml` and lives in both `patches/` here and
   `patches/packages/libvpx/` there, but this host cannot run `mogrix build`
   (§The mogrix pipeline cannot run on this host), so it is rendered-and-checked
   rather than built. The library it produced *was* built and measured — by hand,
   in `ports/work/libvpx-1.13.1`, and copied into staging.

---

## Operating the emulator — the mechanics that cost time

### Boot

Use **`~/iris-upstream/target/release/iris`**, upstream at `02c4e155` ("fix
hostr readback issues"). Anything older wedges the X server within one frame of
capture load. `--cpu` is a runtime option now — the CPU is no longer a build
feature — so there is no private build to maintain and nothing to rebuild.

```
cd ports/iris-run
DISPLAY=:1 ~/iris-upstream/target/release/iris \
    --config iris.toml --ci --ci-display --cpu r5000 > iris.log 2>&1 &
```

**`iris-ci start` is required.** Under `--ci` the CPU thread is created paused;
without `start` the machine sits there doing nothing and `console.log` stays
empty. It looks exactly like a hung boot.

```
export IRIS_SOCKET=/tmp/iris-rdagent.sock        # NOT the default /tmp/iris.sock
~/iris-upstream/target/release/iris-ci start
```

Boot to a usable telnet takes about 5 minutes; **telnetd answers well before the
serial console prints its login banner**, so do not use the banner as the
readiness test — poll `gsh.py 'echo up'` instead.

Then, on the host, in another shell:

```
./serve.sh          # the build and guest/*.sh on :8099
```

and on the guest, once:

```
wget -q http://192.168.0.1:8099/fetch.sh -O /tmp/fetch.sh && chmod 755 /tmp/fetch.sh
sh /tmp/fetch.sh    # pulls the binaries and every helper script
```

after which the whole loop is `sh /tmp/fetch.sh; sh /tmp/run-agent.sh` and
whichever check you want. `iris-ci login root` gets you a serial shell when
telnet will not answer — see the pty note in §Mistakes.

### Talking to the guest

**For anything automated, use the serial console.** `iris-ci run --shell sh
--timeout N`, wrapped as `guest_run` in `scripts/ci-lib.sh`. It is slower than
telnet and it has never once failed; telnet stalls every few dozen sessions in a
way that is indistinguishable from a wedged guest and cannot be fixed from
inside (see §Mistakes and `docs/ISSUE-nat-inbound-stall.md`). The whole
packaging pipeline runs on it.

Two rules that are not optional there. **`--shell sh`**, because the guest's
root shell is bash and iris-ci defaults to asking csh for `$status` — so every
command comes back "guest exit -1" and the exit code is a lie. And **one short
command per line**: a long one comes back as several hundred bytes of garbage
and a syntax error on something nobody typed. Anything long goes in a script,
fetched over HTTP — which works, because guest-to-host is the direction that has
never broken.

For interactive work, telnet is still much nicer while it is answering. Use
**`ports/iris-run/gsh.py`**, not `irixsh.py`. It brackets every command with
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

### What changed on the disk, 2026-08-24 (the packaging session)

**Nothing.** `~/Indy-IRIX65_dev.chd` is byte-for-byte the image the previous two
sessions left, and no sidecar is pending.

This session's diff was test churn again, and this time it included **an
installed copy of the package** — `/usr/sbin/rustdesk-agent`,
`/usr/sbin/rustdesk-agent-gui`, `/usr/lib/rustdesk-agent/`, the Toolchest
fragment and inst's own `/var/inst` bookkeeping — put there three times over by
`scripts/iris-install-test.sh`. It was **discarded**, deliberately, for two
reasons beyond the usual one:

- It is reproducible in one command, from a version stamped `-dirty` that will
  be superseded by the next build.
- **An installed agent in the base image is a trap for the next session.** The
  helper prefers `/usr/sbin/rustdesk-agent` over `/tmp/rustdesk-agent`, by
  design — so a stale installed copy would silently become the thing every test
  runs, which is a version of exactly the confusion that §THE BUTTONS WORK
  describes finding.

If you *do* want it permanent, install the tardist and fold the diff; the
procedure is above and nothing about the package makes it special.

The guest was halted cleanly (`echo yes | /etc/halt` over the serial console,
waited for "Okay to power off the system now."), the emulator killed by pid, and
the 184 MB diff moved aside rather than deleted, in case it is wanted:
`/tmp/claude-.../scratchpad/discarded-diff-20260824.chd`. It will not survive a
reboot of the host, which is the intention. The base image is
`md5 0604bd26d8f8c0801e5a8778c3834b78`, dated 2026-08-19, unchanged.

Bringing the guest back is the usual two commands — see §Boot — and
`serve.sh` plus one `sh /tmp/fetch.sh` on the guest puts the whole harness back
in a minute.

### What changed on the disk, 2026-08-23 (the performance session)

**Nothing.** `~/Indy-IRIX65_dev.chd` is byte-for-byte the image the previous
session verified and left, and no sidecar is pending.

The session's diff was 106 MB of pure test churn — `/tmp` binaries, the guest
helper scripts, `SYSLOG`, `wtmp`, the XFS log — and was **discarded**, not
folded, which is what the previous session did with the same kind of diff. The
helper scripts are not lost with it: they now live in git at
`ports/iris-run/guest/` and `serve.sh` copies them into the build directory, so
`fetch.sh` puts them back on the guest in one command.

The guest was halted cleanly (`echo yes | /etc/halt`, waited for "Okay to power
off"), the emulator killed by pid, and the diff moved aside rather than deleted
in case it is wanted:
`/tmp/claude-.../scratchpad/discarded-diff-20260823.chd`. It will not survive a
reboot of the host, which is the intention.

Two things on the guest are worth folding *if* someone wants them permanent, and
neither was: `/tmp/*.sh` (the harness, but it is in git and refetched in one
step) and nothing else. There is no reason to write to this image at present.

### What changed on the disk, 2026-08-19

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

Disk cost of the working set, for when space gets tight: `ports/rust` 2.1 GB
(private toolchain + registry), `ports/work` 137 MB (third-party source trees,
gitignored — but note `ports/work/libvpx-1.13.1` now holds the **patched**
libvpx that staging was built from, so deleting it means re-applying
`patches/libvpx-vp8-active-map-early-out.patch` and rebuilding),
`~/iris-upstream/target` 1.2 GB (the emulator, not ours to delete lightly).

---

## Mistakes not to repeat

**A wedged X server hangs, it does not fail.** It accepts the connection and
never replies, so every naive readiness check waits for ever: `xdpyinfo` has no
timeout of its own. The boot script's first readiness probe was
`xdpyinfo -display :0 > /dev/null 2>&1` in a loop, and against a wedged server
it started nothing at all and left a pile of stuck xdpyinfo processes behind.
Bound every X probe. `/root/tmo 25 xdpyinfo` is the guest's version of the same
lesson, further up this file.

**`nohup` cannot see a shell function.** `nohup some_function &` silently does
nothing -- no error, no process. A subshell inherits functions, so
`( trap '' 1 2 3; some_function ) &` is the form that works in SVR4 sh.


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
- **A telnet session that negotiates and then never prints a login prompt has
  two causes, and the second one is not fixable from the guest.** The first is
  ptys: IRIX has eleven `/dev/ttyq*`, `gsh.py` used to drop its socket without
  logging out, and after a few dozen runs telnetd could not allocate one. `who`
  on the serial console shows it, `guest/cleanpty.sh` reaps them, and `gsh.py`
  now sends `exit` in a `finally`.

  The second looks identical and is not that. During the packaging session the
  guest was listening on `*.23`, `/etc/inetd.conf` had telnet enabled, `who`
  showed one session, SYSLOG said nothing, and the guest could still `wget` from
  the host — outbound NAT fine, inbound dead. Restarting inetd cleared it once
  and did nothing the next time. It is written up in
  `docs/ISSUE-nat-inbound-stall.md`. **The lesson for anything automated: use
  the serial console.** It has never failed, `iris-ci run --shell sh` makes it
  usable, and the packaging pipeline uses nothing else.
- **`iris-ci get` needs a shell on the serial console**, and hangs for its whole
  timeout if the console is sitting at a login prompt. `iris-ci login root`
  first. For small answers it is usually cheaper not to move the file at all:
  `dd bs=1 skip=N count=3 | od -An -tu1` reads a pixel out of a PPM over telnet.
- **`ps | grep <name>` matches the shell running the grep**, and on IRIX so does
  `pgrep -f`. This bit again this session: `pgrep -f "http.server 8099"` matched
  its own `bash -c` line and reported a server that was not running.
- **A libvpx build tree carries the absolute path it was configured at.**
  `libs-generic-gnu.mk` had `SRC_PATH` pointing at `~/repos/irix-rustdeskagent`,
  which no longer exists, and `make` worked anyway until something needed
  `libvpx_g.a` rebuilt — at which point it failed on a missing `libs.mk` rather
  than on anything to do with the actual problem.
- **Measure the encoder per frame, not per configuration.** `pipeline` averaged
  two rounds, one of them a forced keyframe. That single decision hid the largest
  fact about this encoder — a keyframe costs three to four times an inter frame —
  and an average of the two describes neither. `perfprobe encode` reports each.
- **A/B on a shared host has to be interleaved.** The first attempt at measuring
  the libvpx patch ran stock then patched, and reported a 3x improvement at one
  scale and none at another. That is not a result, it is the load average moving.
  Three interleaved rounds gave a clean and consistent answer.
- **A frame rate measured against fixed content is the rate of change, not the
  ceiling.** A screen that changes once a second reports one frame a second
  however fast the agent is. Two workloads in a row measured the *test*: an
  xterm typing a word a second gave 1.5 fps, and a peer driving the pointer gave
  exactly one move per frame (302 moves, 301 frames) because its own read
  timeout silently does not work on IRIX. Drive the input from a `poll(2)` clock
  and read the ceiling off the per-frame line, not off the total.
- **Two emulators on one CHD, twice now.** `kill -TERM` on iris does not always
  take; check with `ps` before starting another. The new one says so in its log
  -- `TCP port forward 127.0.0.1:2324 failed to bind: Address already in use` --
  and that line is the only warning you get.
- **`ps -e` truncates the command to EIGHT characters.** This is in the list
  twice now because it bit twice, in two different files, and the second time it
  was inside `agent-helper.sh` where it made every lifecycle button appear
  broken (§THE BUTTONS WORK). `ps -e -o pid,args` carries the full command line.
  And `grep rustdesk-agent` against that matches the settings panel too, so a
  kill loop written without `grep -v rustdesk-agent-gui` takes the window down
  with the agent.
- **A wedged X server looks like a script that never ran.** `gui-press.sh`
  produced not one line of output for fifteen minutes and looked like a shell
  that had failed to start. It had started: `xsetroot` was the first thing it
  ran and a wedged Xsgi never answers, so it blocked before the first `echo`.
  Any script that touches the display should prove the server answers *first* —
  `/root/tmo 25 xdpyinfo` — and say so, because the alternative is debugging the
  wrong program. `/root/restart-x.sh` is the way back.
- **A sourced file cannot find itself under `/bin/sh`.** `env.sh` resolved its
  own directory from `${BASH_SOURCE}`, which is correct when a person sources it
  and empty when a `#!/bin/sh` script does — where `$0` names the *calling*
  script, so the fallback silently resolved to the wrong tree and cargo failed
  somewhere unrelated. The caller passes `RD_RUST_DIR` instead.
- **The console shell keeps its working directory between commands.** Convenient
  for `cd` on its own line; fatal for `rm -rf /tmp/gd` when the last run left the
  shell inside `/tmp/gd`. "Cannot remove the current working directory" under
  `set -e` stops a pipeline on its second run and never on its first.
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
                 SO_RCVTIMEO on IRIX; platform string "Linux" on IRIX.
                 2026-08-23: a scale factor through `Video` and out to
                 `PeerInfo`/`SwitchDisplay`; the active map built from the damage
                 rectangles; the rolling refresh; `image_quality` acted on; the
                 settle repaint and the wall-clock keyframe rule switched off on
                 IRIX only. The Mac's paths are the `#[cfg(not(irix))]` arm of
                 each and are byte-for-byte what they were.
src/main.rs      gates widened to any(macos, irix); the `--probe-display` sweep
                 gained the profile and min_q rows
src/input.rs     gates widened to any(macos, irix) so the shim is used;
                 `Injector::set_display_scale`, default 1
src/cursor.rs    `Tracker::set_scale`, default 1
src/convert.rs   channel order by platform (`CH`) -- IRIX is A,B,G,R
src/png.rs       the same, so screenshots are not red/blue swapped either
src/encode.rs    `Tune::profile` and `Tune::min_q`; the active-map API;
                 `Decoder`, for checking the picture rather than the pipeline
src/vpx_shim.c   the active map, the profile and quantizer floor, and a VP8
                 decoder
src/sys.rs       added wait_readable (IRIX only)
```

**Nothing here changes what macOS builds.** `any(macos, irix)` reduces to
`macos` there by definition; every IRIX behaviour change is behind
`cfg(target_os = "irix")` with the previous code as the `not(irix)` arm; the two
new setters default to 1 and the Mac never calls them; and `Tune`'s new fields
default to the values the shim used before. The host test suite went from 188 to
196 passing, with the six new tests covering exactly the pure logic that was
added (`clamp_scale`, the injector's scaling, the tracker's).

`~/repos/mogrix` on `danifunker-ports`, uncommitted:

```
M .gitignore                                   anchored /lib/ and /lib64/
M compat/include/mogrix-compat/generic/time.h  CLOCK_MONOTONIC -> CLOCK_SGI_CYCLE
M cross/bin/irix-ld                            LLD path was hardcoded to /home/edodd
M pyproject.toml                               mcm-engine pinned to a local path
M scripts/build-runtime-objects.sh             -O2 -fno-builtin for safe_mem
M scripts/patch-rust-sysroot.sh    NEW TONIGHT nine patterns for the 2026-08 std
                                               layout, plus tolerate a missing file
M mogrix/crate_patcher.py                       REGISTRY_BASE honours CARGO_HOME
M rules/packages/libvpx.yaml       NEW 08-23    lists the active-map patch
?? cross/lib/{dso_handle,safe_mem}.c, irix-shared.lds
?? rules/packages/{libsodium,libvpx,mbedtls}.yaml
?? patches/packages/{libvpx,mbedtls}/
?? patches/packages/libvpx/libvpx-vp8-active-map-early-out.patch  NEW 08-23
```

Left uncommitted deliberately, matching how the rest of the mogrix work here has
been handled. The libvpx patch also lives in this repo at
`patches/libvpx-vp8-active-map-early-out.patch`, so it is not only in an
uncommitted tree — but the mogrix copy is the one that would make it
reproducible, and it has **not** been through a mogrix session with the MCP
knowledge server connected, which its `CLAUDE.md` requires.

**Still missing from a fresh mogrix clone** (Dani should ask unxmaal):
`compat/runtime/*` (gitignored) and `cross/lib/elf_utils.py`. `soft_float_stubs.c`
is a reconstruction that **aborts by name rather than forwarding** to IRIX's
`__q_add`/`__q_mul`; `safe_mem.c` is also a reconstruction.

The `mbedtls`/`libsodium`/`libvpx` rules still have **not** been through a mogrix
session with the MCP knowledge server connected, which its `CLAUDE.md` requires
(`add_rule`, `report_error`). Flag for Dani; do not fake it. The stray empty file
`~/repos/mogrix/a` is Dani's to remove.
