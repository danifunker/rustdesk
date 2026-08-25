# RESUME — RustDesk agent for Solaris 10 / SPARC

State of the port, the environment it runs in, what is already proved and what
is not, and the traps that cost time the first time. This is the source of
truth; `RESUME-PROMPT.md` is the short prompt that points here.

Last updated at the end of the session that got VP8 encoding on the machine.

---

## 1. The machine

**Sun Blade 2500**, `192.168.99.176`, user `dani` (uid 100, group `staff`), ssh
keys already work. Solaris 10 8/11 (`s10s_u10wos_17b`), one UltraSPARC IIIi at
**1600 MHz**, 4 GB RAM, ~15 GB free on `/`. XVR-600 framebuffer at
`/dev/fbs/jfb0`.

**No root.** `sudo` exists at `/opt/csw/bin/sudo` but wants a password;
`pfexec` is present without a profile. Ask for anything that needs it.

### Toolchain there

| | |
|---|---|
| C compiler | **`/opt/csw/bin/gcc-5.5`** (5.5.0) — required |
| also present | gcc 4.9.2 (`/opt/csw/bin/gcc`), g++ 4.9.2, gcc 3.4.3 (`/usr/sfw/bin/gcc`) |
| linker / ar | `/usr/ccs/bin/ld` (Solaris 5.10), `/usr/ccs/bin/ar`, GNU `ar` as `/opt/csw/bin/gar` |
| GNU userland | under **`/opt/csw/gnu`** (plain names: `diff`, …) |
| build tools | `gmake`, `gtar`, `rsync`, `curl`, `screen`, `python` (2.x), `bash` **3.00** at `/usr/bin/bash` |

gcc **4.9 is not enough**: it has no `__builtin_add_overflow`/`sub`/`mul`,
which mrustc emits for Rust's checked arithmetic. They arrived in gcc 5, and
4.9 compiles them as implicit declarations — the failure is three undefined
symbols at link time and nothing before it.

`cc1` is a 32-bit SPARC32PLUS binary, so it is capped near 4 GB of address
space however much RAM the machine has. Not a problem so far (libstd's 31 MB
unit goes through); `SPARC_BIG_TU_BYTES` in the ssh wrapper is the lever.

### X on the machine

The console is **not usable** and needs root to become so: `/dev/fbs/jfb0` is
`crw------- root root`, `Xsun` is not setuid, and while CDE sits at its greeter
`dtgreet` holds a server grab that hangs every client *inside `XOpenDisplay`*
(confirmed with `pstack`: blocked in `_XWaitForReadable` during the connection
handshake — not an auth failure, no error, just silence).

So the agent gets its own session: **`scripts/rd-session.sh start`** brings up
`Xvfb :2` + `dtwm` + `dtterm` — a real CDE desktop, startable by an ordinary
user, surviving logouts. `:0` is the console, `:1` is left for ad-hoc probes.
Use `/usr/X11/bin/Xvfb`; Sun's own `/usr/openwin/bin/Xvfb` has no `-screen`.

`Xsun` itself advertises DAMAGE, XFIXES, MIT-SHM, XTEST, RENDER, SHAPE, RECORD,
SYNC, XKEYBOARD, DOUBLE-BUFFER, SUN_OVL, XC-MISC (read out of the binary with
`strings`). **X client libraries live in two prefixes**: Xlib/Xext/Xtst under
`/usr/openwin/lib`, but **Xfixes and Xdamage under `/usr/openwin/sfw/lib`** —
both with `sparcv9/` builds, all sharing `/usr/openwin/include`. Looking only in
the first says the machine has no XFIXES, which is wrong.

### C libraries built for sparcv9

All in **`~/sparc-deps`** on the Blade, static, `-m64 -O2 -fPIC`, built by
`scripts/build-deps.sh` (tarballs staged in `/tmp` there):

libsodium 1.0.18 · zstd 1.5.6 · mbedTLS 3.6.2 · libvpx 1.13.1

Sources are fetched on the build host and copied over: Solaris 10's curl
carries a certificate store from 2011.

---

## 2. The build host

`/home/dani/repos/mrustc` — branch **`sparc-solaris-10`**, four commits:

```
24e0ed0a  hir: Support `#[repr(align(N))]` on unions
400e373a  trans: Add sparcv9-sun-solaris
3587bd55  minicargo: Allow STD_ENV_ARCH to be overridden
109ddad1  expand: Fix `{:02x?}` leaving "?}" in the formatted output
```

`/home/dani/repos/rustdesk` — the agent in `rustdesk-sparc-agent/`, plus
Solaris arms added to the shared PowerPC sources (`session.rs`, `main.rs`,
`input.rs`, `cursor.rs`, `sys.rs`).

### Building the standard library (once)

```sh
cd ~/repos/mrustc
MRUSTC_TARGET_VER=1.74 SPARC_HOST=192.168.99.176 SPARC_CC=/opt/csw/bin/gcc-5.5 \
CC_sparcv9_sun_solaris=~/repos/rustdesk/rustdesk-sparc-agent/scripts/sparc-cc-remote.py \
  make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 \
       MRUSTC_TARGET=sparcv9-sun-solaris OVERRIDE_SUFFIX=-solaris \
       STD_ENV_ARCH=sparc64 PARLEVEL=2
```

Lands in `output-1.74.0-sparcv9-sun-solaris`. About 9 minutes of Blade CPU for
the C half (libcore 222 s, libstd 123 s) plus the front end here.
`scripts/recompile-objects.sh` replays just the codegen when only the C
compiler changes.

**The union `repr(align)` change altered rlib metadata.** The host tree was
rebuilt for it; `output-1.74.0-powerpc-apple-darwin*` and `output-1.74.0-g[345]`
were **not**, and will fail with `Bad tag for HIR::TypeItem` if used with the
current mrustc. Rebuild them before the next PowerPC build.

### Building the agent

```sh
cd ~/repos/rustdesk/rustdesk-sparc-agent
SPARC_HOST=192.168.99.176 ./scripts/build-sparc.sh
SPARC_HOST=192.168.99.176 ./scripts/build-sparc.sh run captest --frames 5
```

Rust is compiled here, the C it becomes is compiled on the Blade over ssh by
`scripts/sparc-cc-remote.py`, and the binary comes back. No cross toolchain is
involved. A build cycle is roughly 8–12 minutes.

---

## 3. What is proved, and how

Every number below was measured on the Blade, not inferred.

**Rust itself** (`probes/smoke.rs`, built with `-C panic=unwind`):

```
os=solaris arch=sparc64 pointer_width=64
native bytes of 0x01020304 = [01, 02, 03, 04] (big-endian)
u128 align=16 big/7=16203922234330403022065457496750867212
atomics: u8=232 (expect 232 after wrap) u64=1000 mutex=1000
catch_unwind: unwound OK
slept 120.063999ms; bound 127.0.0.1:33385; file round-trip ok
```

mrustc defaults to `panic=abort`; without `-C panic=unwind` a panic kills the
process instead of unwinding.

**The portable half** (`prototest`, one binary, all of it checked against bytes
rather than against itself):

```
RegisterPeer wire bytes: 0a 0b 73 70 61 72 63 2d 62 6c 61 64 65 10 84 86 88 08
PNG: 75 bytes, IHDR says 4x2      (carried back here: chunk CRCs valid, pixels right)
white 16x16 -> Y=235 U=128 V=128
secretbox: sealed 38 bytes, opened them again
ed25519: signed IdPk verifies and parses, 111 bytes, pk 32 bytes
login hash: 32 bytes, stable, and discriminating
vp8: keyframe 1221 bytes, first bytes 10 3b 00 9d 01 2a
cpu: "SUNW,Sun-Blade-2500 @ 1.6 GHz"  memory: "4 GB"  os: "Solaris 10"
```

**Capture** (`captest`, against `Xvfb :2` at 1280×1024 with `xclock -update 1`):

| | DAMAGE + MIT-SHM | MIT-SHM + comparison |
|---|---|---|
| full screen read | 4.9 ms | 4.9 ms |
| 20 idle polls | **1.4 ms total** | 168 ms total |
| a change reports | `181x181 at 51,51` | a band, `1280x64 at 0,96` |

Pixel order is **A,R,G,B** (the PowerPC Mac's, *not* the IRIX port's A,B,G,R),
stride is exactly `width*4`, `XGetImage` costs 200 ms against SHM's 4.9.

**Input** (`probes/inputtest.c`): pointer moves land exactly, buttons work,
typing reaches the focused window; typing into the terminal produces damage
rectangles of `6x13 at 93,90` — one character cell.

**Cursor** (`probes/cursortest.c`): a 9×16 I-beam with hotspot 4,8 over dtterm,
drawn as text so a decoding error is visible rather than merely suspected.

**Clipboard** (`probes/cliptest.c`, two processes): 21 bytes transferred over a
real SelectionRequest, and exactly one change reported when ownership moves.

---

## 4. The next task, in detail

`session.rs` is fully wired with Solaris arms and compiles until it reaches
four capture methods this port does not have. They are the only thing between
here and a running agent.

Held out of `src/lib.rs` for that reason: `session`, `clipboard`, `api`,
`rendezvous`, and the `rustdesk-agent` binary (commented out in `Cargo.toml`).

### 4a. Three easy ones — copy the IRIX versions

`rustdesk-irix65-agent/src/capture.rs` has all three, and they use only what
this port's shim already provides. Add a `forced: Vec<bool>` field alongside
them.

* `invalidate_band(&mut self, b: usize)` — set `forced[b]` (IRIX line ~386).
* `read_band(&mut self, b: usize) -> (usize, usize)` — return `band_range(b)`,
  and when `forced[b]`, first `rd_capture_read_rect(0, start, width, end-start)`
  and clear the flag (IRIX line ~397).
* `refresh(&mut self) -> bool` — re-read the geometry, because the screen
  resolution can change under a running session (IRIX line ~419).

### 4b. The real one: `to_i420_rect`

```rust
pub fn to_i420_rect(&self, img: &mut crate::convert::I420, factor: i32,
                    dx0: i32, dy0: i32, dx1: i32, dy1: i32) -> bool
```

The IRIX wrapper is `capture.rs:511`; its C inner loop is
`rd_abgr_to_i420_rect` at `capture_shim.c:769`, with `rd_scale_abgr` at 698.

**It cannot be reused as-is, and must not be renamed into place.** That loop
reads **A,B,G,R**; this machine's canvas is **A,R,G,B**. Rename it and every
frame has red and blue swapped, which is miserable to spot over a remote
display and looks like a client bug.

Write `rd_argb_to_i420_rect` in `src/capture_shim.c` with the same contract as
the IRIX one, whose header comment (`capture_shim.h`, the long block above the
declaration) explains why each part is the way it is:

* One walk of the canvas, not a downscale followed by a conversion — on the
  Indy those two steps cost 386 + 306 ms with a whole scaled framebuffer of
  memory traffic between them.
* A **destination rectangle**, so a frame costs what the damage costs rather
  than what the screen costs.
* `factor` must be a power of two (1, 2, 4, 8): that is what lets the box
  average fold into the BT.601 weights as a shift instead of a per-pixel
  divide.
* `dx0/dy0/dx1/dy1` are **destination** pixels, half-open, snapped outward to
  even — a 4:2:0 chroma sample is shared by a 2×2 destination block, and a
  bound falling inside one would leave half of it unwritten.

Test it before believing it: `convert::argb_to_i420` already works here
(prototest checks white → Y=235, U=V=128), so encode the same synthetic image
both ways at `factor = 1` and compare the planes. Then check a red and a blue
patch specifically, since that is the failure this is guarding against.

### 4c. Then

Wire `clipboard`, `api`, `rendezvous`, `session` into `lib.rs`, uncomment the
`rustdesk-agent` binary in `Cargo.toml`, and iterate. Expect more gaps: 2,400
lines of session code have only ever been compiled for two platforms. The
pattern that has worked is to add `target_os = "solaris"` beside
`target_os = "irix"`, **except** where the IRIX arm exists for something IRIX
lacks — the two `SO_RCVTIMEO` sites in `session.rs` are deliberately not
shared, because Solaris has the socket option and takes the ordinary path.

Watch for the negated form: `cfg(any(macos, irix))` and
`cfg(not(any(macos, irix)))` are different strings, and updating one without
the other leaves both arms of `pump_input!` live at once. The error is an arity
mismatch on a function whose parameters are themselves cfg-gated.

---

## 5. After that, in order

1. **Run it against a real client**, direct-IP first (`--port 21118`, identity
   from `--show-key`). Nothing is proved end to end until this works.
2. **Tune for this machine.** Completely unmeasured. `DEFAULT_SCALE` is 2 on
   IRIX and 1 elsewhere; the encoder `Tune` (profile 3, screen_content 2) is
   inherited from IRIX on a guess. VP8 at 1280×1024 on a 1.6 GHz UltraSPARC
   IIIi may want the halved size, or may not — measure, do not assume.
3. **The Motif settings panel**, following `rustdesk-irix65-agent/gui/`: a
   *second binary* with no behaviour of its own, each button running one verb
   of a helper script that runs one `rustdesk-agent --flag`. Solaris has Motif
   **2.1.0** with 64-bit libraries at `/usr/dt/lib/sparcv9/libXm.so` and
   headers in `/usr/dt/include/Xm`; the IRIX panel is Motif 1.2 code, which 2.1
   accepts.
4. **Packaging.** Nothing exists. The IRIX port's `inst/` and release script
   are the model; Solaris wants a `pkgadd` datastream.

---

## 6. Loose ends

* **No scroll wheel.** The pointer has three buttons, so X buttons 4 and 5
  cannot be injected — `XTestFakeButtonEvent(5)` is a `BadValue`, not a no-op.
  The shim reads the button count at startup, declines, and says so once. If
  the console session (a real mouse) has five, this resolves itself there.
* **The console (`:0`).** Re-run `probes/xprobe.c` and `captest` against it
  when it becomes usable: a real framebuffer read will not cost what Xvfb's
  does, and the whole capture design rests on that number.
* **INCR clipboard transfers** (over 256 KB) are reported as empty rather than
  as a truncated fragment. Implementing INCR is the fix if it matters.
* **`SO_RCVTIMEO` on Solaris is assumed, not measured.** The session takes the
  non-IRIX path; if a session stalls between bands, that is the first suspect.
* **The PowerPC output trees are stale** (see §2) and will fail loudly.
* **`Xvfb :1`** is still running from an ad-hoc probe and is not managed by
  `rd-session.sh`; `kill` it when convenient.

---

## 7. Traps, all of which cost time once

**Solaris tooling**

* `/usr/bin/grep` has no `-q`, no `-r`, and no `\|` alternation. Use `egrep`,
  and `find … -exec grep`. A pattern with `\|` matches *nothing* and reports
  success, which is how a running `Xsun` was declared absent.
* There is no `timeout(1)` and no `head -c`. `tail -r` does exist.
* `pkill -f xprobe` kills the ssh command running it, because that command line
  contains the pattern. Match by process name.
* An ssh command runs a **non-login shell** whose PATH has neither
  `/opt/csw/bin` nor rsync. The wrapper sets `SPARC_REMOTE_PATH`.
* `/bin/sh` is the Bourne shell: no `$( )`, no `local`, no `[[ ]]`. `bash` is
  **3.00** — no `mapfile`, no `${var,,}`, no associative arrays.
* Grouping matters: `a || b | while read …` parses as `a || (b | while …)`.
  That is exactly how `rd-session.sh stop` came to kill nothing.

**mrustc / minicargo**

* **minicargo does not honour `cargo:rerun-if-changed`.** An edited `.c` links
  against the stale archive and the build *succeeds* — several measurements
  were of the old object before this was found. `build-sparc.sh` drops the
  build-script cache when a shim is newer.
* mrustc defaults to `panic=abort`.
* `cc-rs` does not know this triple, and gcc on Solaris defaults to 32-bit
  sparc, so the wrapper pins `-m64`. Without it the linker says "wrong ELF
  class: ELFCLASS64" about an object that looks fine.
* `STD_ENV_ARCH` must be passed for a cross build or `std::env::consts::ARCH`
  reports the host's.

**Libraries**

* `libc` binds `sysconf` and `pthread_kill` to `__sysconf_xpg7` and
  `__pthread_kill_xpg7`, which arrived in **Solaris 11.4**. `src/compat_shim.c`
  forwards them. Every other `link_name` symbol libc uses does resolve here —
  the way to check is to ask the linker, not `nm`.
* libsodium is built with the stack protector and its runtime lives in gcc's
  `libssp` here, not libc. Linked statically from `/opt/csw/lib/sparcv9`.
* zstd needs `ZSTD_NO_ASM=1` or it assembles x86-64.
* libvpx needs GNU `diff` on PATH, `LD` set as well as `CC`, its helper-script
  shebangs pointed at bash, and its one C++ file (the RTC rate controller,
  which the agent does not call) removed — `pkgutil -i gcc5g++` is the
  alternative to removing it.

**X**

* `XFixesCursorImage.pixels` is `unsigned long *` — **8 bytes per pixel** here,
  holding a 32-bit ARGB value. Read as packed u32 and every second pixel is
  wrong.
* Xlib's default protocol-error handler calls `exit()`. Both shims install one
  that reports and continues; a rejected input event must not end a session.
* A damage object on the root **does** report child windows here, but do not
  assume it: the capture path checks itself against the canvas every two idle
  seconds and downgrades permanently if damage was lying. A synthetic
  startup probe was tried and answered wrongly on both servers it ran against.
* Default `xclock` redraws once a *minute*. Use `-update 1` when testing
  damage, or conclude the wrong thing (this happened).

---

## 8. File map

```
rustdesk-sparc-agent/
  src/capture_shim.{c,h}   MIT-SHM capture, DAMAGE, the hash fallback
  src/capture.rs           its Rust side (missing the four methods in §4)
  src/input_shim.c         XTEST injection, from the IRIX shim via libXtst
  src/cursor_shim.c        XFIXES cursor
  src/clipboard_shim.c     X selections, both directions
  src/compat_shim.c        the two Solaris 11 symbols this machine lacks
  src/lib.rs               what is wired, and what is not, with reasons
  src/bin/captest.rs       capture, alone
  src/bin/prototest.rs     the portable half, checked against bytes
  probes/                  sysprobe.sh, xprobe.c, shimtest.c, inputtest.c,
                           cursortest.c, cliptest.c, smoke.rs
  scripts/build-sparc.sh   build the agent (needs SPARC_HOST)
  scripts/build-deps.sh    the C libraries, on the Blade
  scripts/rd-session.sh    the desktop the agent serves
  scripts/sparc-cc-remote.py   the ssh "compiler"
  scripts/recompile-objects.sh replay codegen after a compiler change
```

The portable modules are **not** copied here: they come from
`../rustdesk-ppc-agent/src` by `#[path]`, as the IRIX port takes them.
