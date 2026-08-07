# Building

One command, once both machines are set up:

```bash
./build-release.sh --host admin@192.168.99.116
```

That produces `target/release/<version>/` with **one** `.dmg` and one
`.tar.gz`, plus `SHA256SUMS` and a `MANIFEST.txt` saying what was verified and
what was not.

Everything below is about getting to the point where that command works.

---

## One build, targeting the oldest hardware

There is a single artifact and it is compiled for the **G4** (`-mcpu=7450`,
which stamps `cpusubtype` 10 / `ppc7400`). It runs on a G4, on a G5, and under
Rosetta. Two reasons, in order of importance:

* **Rosetta refuses anything requiring a G5.** A `ppc970`-stamped binary cannot
  run under it at all, so targeting the G5 would rule out Intel Macs running
  10.6 with Rosetta installed — see "Mac OS X 10.6" below.
* **The G5 build buys ~3 ms on an idle poll and nothing on a frame.** Both were
  run on the G5 and compared directly, five `--probe-display` runs each,
  alternating:

  | | `argb->rgb` | `argb->i420` | idle change-detection probe |
  |---|---|---|---|
  | G4 build | 16-17 ms | 20 ms | 106 ms (106,106,106,106,107) |
  | G5 build | 16-17 ms | 19-20 ms | 103 ms (103,103,103,105,110) |

  The two conversions — the frame path — are indistinguishable, which figures:
  they are C shims, and libvpx, libsodium, libyuv and libopus are all already
  generic `ppc`, so the encoder is *literally the same object code* in both.
  The change-detection probe is consistently ~3 ms (3%) slower on the G4 build,
  which is real rather than noise; it is integer sampling across the framebuffer
  and is where `-mpowerpc64` plausibly helps. That is 3 ms on the cost paid when
  nothing is happening, and under 1% of a ~405 ms full-screen frame.

  Whole sessions were compared the same way, two rounds alternating: 16 frames
  and 2 keyframes every time, first frame 0.83-0.91 s on both, and the
  screenshots byte-identical within each round — same pixels in, same bytes out.

**The G4 build was then run on the G5 through a whole session**, rather than
being reasoned about: secure handshake, login, 16 video frames with 2 keyframes,
first frame 0.87 s, the 1.2.4+ refresh button producing a keyframe in 0.72 s
with the control correctly producing none, and a 1920x1080 screenshot in 0.75 s.
`--probe-display` on the same binary reports the C colour shim byte-identical to
the Rust reference for both `argb->rgb` and `argb->i420`. Those numbers match
what the G5 build has always produced.

The G5 and universal (fat, `lipo`-fused) paths still work and are one flag
away — `--arch "g5 g4 universal"` — but nothing needs them, and each extra
variant is another download for someone to pick wrongly.

**A G3 will not work.** The shims and libvpx use AltiVec, which no G3 has. See
`BACKLOG.md` §13.

---

## Why two machines

**You cannot build this on one computer, and the reason is not convenience.**

mrustc does not emit machine code. It translates Rust to C, and that C has to be
compiled by a C compiler targeting `powerpc-apple-darwin` — of which there is no
usable cross-build. So `build-ppc.sh` runs mrustc on the Linux host and ships
every translation unit to the Mac over ssh, where `gcc` compiles it and the
object comes back. `ppc-cc-remote.py` is that wrapper; minicargo is told it is
`CC_powerpc_apple_darwin` and never knows the difference.

Four more steps are Darwin's alone and equally not portable:

| step | tool | why it cannot move |
|---|---|---|
| relocating the bundled libraries | `install_name_tool`, `otool` | cctools; no Linux build here |
| fusing the G4 and G5 builds | `lipo` | same |
| compiling the settings app | Apple `gcc` + Cocoa | Objective-C against 10.5 frameworks |
| building the disk image | `hdiutil` + the Finder | HFS+ image, and the window layout is set by AppleScript |

So the split is: **the host drives, the Mac compiles and packages.** The host
never needs a PowerPC compiler and the Mac never needs Rust.

---

## The host (Linux)

Where you run `build-release.sh`. Tested on x86-64 Linux; nothing here is
architecture-specific.

### Required

| what | why | checked against |
|---|---|---|
| **mrustc + minicargo**, patched | translates the agent's Rust to C | commit `a54c2e38` on branch `ppc-upstream` |
| **per-CPU standard libraries** | mrustc's libcore/liballoc, built once per CPU | `output-1.74.0-powerpc-apple-darwin-{g3,g4,g5}`, ~98 MB each |
| **`ppc-cc-remote.py`, `ppc-ar-remote.py`** | send C to the Mac and bring objects back | `$PPC_TOOLS_DIR/scripts/` |
| **`ppc-compat.c`** | the platform shim linked into every build | `$PPC_TOOLS_DIR/rb-cli-ppc/shim/` |
| **cargo / rustc** | the host test suite, and `regen-protos.sh` | 1.95.0 |
| **ssh, scp, rsync, tar, od, awk, sha256sum** | the plumbing | any |

**mrustc needs two patches** or the build fails outright. Both are on the
`ppc-upstream` branch and both are independently useful — see `BACKLOG.md`
item 10 for why they have not been proposed upstream yet:

* `4504ca61` — minicargo drops the semver pre-release suffix from
  `CARGO_PKG_VERSION`, which breaks crates that parse their own version;
* `a54c2e38` — the Darwin/PowerPC member-alignment cap has to apply to enum
  unions too, or layouts silently disagree with what the C compiler produces.

**The standard libraries are not interchangeable.** mrustc compiles libcore and
liballoc to C and then to Mach-O objects, so each carries the CPU flags it was
built with: the G5 `liballoc` has 863 `rldicl`, 698 `std` and 685 `ld` in it,
and the G4 one has none. Linking the G5 copy into a G4 build yields a binary
that is correctly stamped `cpusubtype` 7400 and traps anyway. `build-ppc.sh`
picks the right one from `PPC_CPU_FLAGS`; `build-release.sh` refuses to start if
one is missing.

### Optional

| what | needed for |
|---|---|
| **Python 3 + Pillow** | regenerating `deploy/app.icns` and `deploy/dmg-background.png`. Both are committed, so a build does not need it |

### Paths

Overridable, and checked before anything slow starts:

```bash
MRUSTC_DIR=~/repos/mrustc            # minicargo, and the per-CPU stdlibs
PPC_TOOLS_DIR=~/repos/rusty-backup   # the remote cc/ar wrappers and the shim
PPC_LIBS_DIR=/Users/admin/ppc-libs/lib   # on the MAC: the static libraries
```

---

## The Mac (PowerPC, Mac OS X 10.4 or 10.5)

Reached over ssh. **It needs no Rust and no mrustc** — it is a C compiler, a
linker and a packaging host. Verified against a Power Mac G5 (`ppc970`) running
10.5.8 (9L31a).

### From Xcode 3

Apple's developer tools, which install to `/Developer`:

| tool | used for |
|---|---|
| `gcc` 4.0.1 (`powerpc-apple-darwin9-gcc-4.0.1`) | compiling the Cocoa settings app |
| Cocoa / AppKit frameworks | the same |
| `otool`, `install_name_tool`, `lipo`, `strip`, `ar` (cctools, in `/usr/bin`) | relocating libraries, fusing the two builds, inspecting Mach-O |

### From MacPorts

| what | used for |
|---|---|
| **`gcc10-bootstrap`** (GCC 10.5.0, in `/opt/local/libexec/gcc10-bootstrap`) | compiling the C that mrustc emits. Apple's 4.0.1 is far too old for it |
| `libzstd`, `zlib`, `libMacportsLegacySupport` | linked by the agent; bundled into the `.app` |
| `libatomic`, `libgcc_s` (from gcc10-bootstrap) | the same |

**The shims are built with `-fno-gcse`** because `gcc10-bootstrap` miscompiles
at `-O2` on this target — a register is read on a path that never wrote it. See
`BACKLOG.md` item 1e; `NO_MISCOMPILE` in `build.rs` carries it.

### Built once, by hand

Static libraries the agent links, in `$PPC_LIBS_DIR` (default
`~/ppc-libs/lib`). They are not built by anything here:

| library | notes |
|---|---|
| `libsodium.a` | `--disable-shared --enable-static`, no `-mcpu` |
| `libvpx.a` | VP8 encode; carries hand-written AltiVec |
| `libyuv.a`, `libopus.a` | `libopus` is only needed if the reverted audio work is restored |

**Build them without any `-mcpu` flag.** They are then generic `ppc` and serve
every CPU variant; the measurement is in `BACKLOG.md` §13 — none of them
contains a single 64-bit instruction, which is what makes one set of libraries
usable for both the G4 and the G5 builds.

### From the OS

Present on any 10.4/10.5: `hdiutil`, `plutil`, `defaults`, `osascript`,
`screen`. No `iconutil` (10.7+) and `sips` cannot write `.icns`, which is why
`deploy/make-icns.py` writes the icon container itself.

### ssh

`ssh <host>` must work **without a prompt** — the C compiler wrapper shells out
to it hundreds of times per build. Either keep a key in an agent:

```bash
ssh-agent -a /tmp/ssh-agent-ppc.sock >/dev/null
SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock ssh-add ~/.ssh/id_rsa
```

or hand the key to the build:

```bash
./build-release.sh --host admin@192.168.99.116 --identity ~/.ssh/id_rsa
```

At least **400 MB free in `/tmp`** on the Mac: the universal binary alone is
~40 MB and the build stages several copies of it.

---

## Mac OS X 10.6

**Untested — there is no 10.6 machine here — but nothing is knowingly in the
way.** What can be determined without one:

* **Nothing we call was removed in 10.6.** The framebuffer trio
  (`CGDisplayBaseAddress`, `CGDisplayBytesPerRow`, `CGDisplayBitsPerPixel`) and
  `CGPostMouseEvent` were *deprecated* there, not withdrawn. Upstream's capture
  uses `CGDisplayStream`, which is 10.8+; this agent deliberately does not, and
  the only mention of it in the tree is a comment saying why.
* **`LSMinimumSystemVersion` is 10.4.0** and nothing sets a maximum, so the
  bundle does not refuse to launch.
* **The build targets G4**, which is what Rosetta requires.

The two things most likely to misbehave, worth knowing before trying:

* **The framebuffer read.** `CGDisplayBaseAddress` is exactly the API Apple
  deprecated because it stopped being meaningful on modern graphics stacks. On
  an Intel Mac under Rosetta it is unlikely to hand back a usable framebuffer.
  This degrades rather than breaks: the agent logs `capture unavailable` with
  the whole display state, serves input and clipboard, and retries video every
  five seconds (`VIDEO_RETRY`) — see `BACKLOG.md` item 1d.
* **The cursor shape**, which comes from the private `CGSGetGlobalCursorData`.
  Private calls are exactly the ones that move between releases. A shape that
  cannot be read falls back to the built-in arrow rather than to no pointer.

If you try it, `~/rustdesk-ppc-agent/agent.log` says which of these happened;
the log lines were written for precisely this kind of unknown.
[`10.6-TESTING.md`](10.6-TESTING.md) has the sequence to run, what each failure
looks like, and what to bring back.

---

## Running it

```bash
./build-release.sh --host admin@192.168.99.116   # everything
./build-release.sh -H ppctiger --arch universal  # one variant
./build-release.sh -H ppctiger --version 0.2.0
```

`--host` takes anything ssh does: `user@ip`, a bare hostname, or an ssh config
alias. `--help` lists the rest.

**Roughly 15 minutes per CPU that is not already built**, nearly all of it gcc on
the Mac. The `target/ppc-g5` and `target/ppc-g4` directories are what make a
second run quick — do not delete them casually. One output directory holds
objects for one CPU; `build-ppc.sh` stamps each with its flags and refuses a
mismatch rather than linking half of one architecture into the other.

### What it checks before starting

Both ends, because finding out that `lipo` is missing after two fifteen-minute
builds wastes half an hour. The Mac: reachable without a prompt, Darwin,
PowerPC, every tool above, the static libraries, and the space in `/tmp`. The
host: minicargo, both wrappers, cargo, the committed artwork, and a standard
library for every CPU in the run.

### What it verifies afterwards

Recorded in `MANIFEST.txt` beside the artifacts, along with what it could
**not** verify — there is no G4 or G3 here, so the G4 artifact is checked by
instruction scan and has never been run on a G4.

* the Mach-O `cpusubtype` matches the CPU each artifact claims;
* no 64-bit instructions in any non-G5 artifact — the check that catches a G5
  standard library linked into a G4 build, which nothing else would;
* every bundled library resolves under `@executable_path`, and the packed binary
  runs on the Mac before being packed;
* the agent supports every setting the settings app offers, so a stale binary
  cannot ship inside a newer app;
* the disk image mounts and holds the app, the Applications alias and the
  background.

A failed check fails the build.

---

## The pieces, if you need one on its own

Each is independently runnable and carries its own reasoning in its header.

| script | does |
|---|---|
| `build-ppc.sh` | one CPU, into `target/ppc` (or `PPC_OUT`) |
| `deploy/make-universal.sh` | `lipo`s a G4 and a G5 build into one binary |
| `deploy/bundle.sh` | relocates the libraries, compiles the settings app, assembles `.app`, tars it |
| `deploy/make-dmg.sh` | the drag-to-Applications disk image |
| `deploy/release.sh` | the orchestration `build-release.sh` wraps |
| `deploy/check-cpu-compat.sh` | disassembles a binary and reports instructions a G4 cannot run |
| `deploy/install.sh` | inside the bundle; installs on the target Mac |
| `deploy/make-icns.py`, `deploy/make-dmg-bg.py` | regenerate the committed artwork |
| `regen-protos.sh` | regenerate `src/protos/` when a `.proto` changes |

## Host-only checks

Neither needs the Mac, and both are worth running before a build:

```bash
cargo test                       # 137 tests: protocol, framing, crypto, config
```

**mrustc does not borrow-check**, so the macOS-gated half of the tree is never
seen by a compiler that would reject it. Force the cfg and check it with real
rustc — the recipe is in the README under "Type-checking the platform layer".
Do this before every deploy; it is the only thing standing between a borrow
error and a PowerPC build that compiles it anyway.
