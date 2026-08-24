# Building

Once the prerequisites are in place:

```sh
./ports/rust/build-compat.sh          # once per clone
cd ports/rust/agent-portable
. ../env.sh
cargo +nightly build --release
```

Binaries land in `target/mips-sgi-irix6.5/release/`. Measured on a 6-core
x86-64 host:

| | time |
|---|---|
| Full build after `cargo clean` — 44 crates including `std` | **37 s** (279% CPU, 850 MB peak RSS) |
| Incremental after a Rust edit | **6.4 s** |
| Incremental after a C shim edit | **7.3 s** |

Everything below is about getting to the point where those commands work.

---

## The build needs no emulator

This is a cross-compile: clang-18 plus `ld.lld-irix` targeting
`mips-sgi-irix6.5`. The timings above were measured with no emulator running at
all. `iris` is only needed to *run* what comes out, and on current evidence a
real SGI would be a better place to run it — see the blocker in the README.

What the build *does* need from an IRIX machine is `/opt/irix-sysroot`: headers
and shared libraries pulled off a 6.5.22m installation. That extraction is a
one-time job and is not part of a normal build.

## Prerequisites

### The cross toolchain

From [mogrix](https://github.com/unxmaal/mogrix), which supplies the compiler
wrapper, the compat headers and the linker:

```
/opt/cross/bin                  clang-18 and ld.lld-irix
/opt/irix-sysroot               pulled from the 6.5.22m image
/opt/sgug-staging/usr/sgug      staging; irix-cc lives in bin/
```

`mogrix setup-cross` deploys the staging tree. The IRIX-specific fixes this
agent needed are on the `danifunker-ports` branch.

### Four C libraries, in the staging tree

All statically linked into the agent, so nothing has to be installed on the
target machine:

| | notes |
|---|---|
| **libsodium 1.0.18** | no source patches. Consumers must link `-lpthread` or `sodium_init()` returns −1 while every `crypto_*` call still works |
| **libvpx 1.13.1** | one patch; VP8 only, `--disable-webm-io --disable-libyuv` (both are C++ and drag in the host linker) |
| **mbedTLS 3.6.2** | two patches |
| **zstd 1.5.6** | no patches; `clipboard.rs` links it |

**These are currently built by hand into `/opt/sgug-staging`, which is outside
every git tree — a real reproducibility gap rather than a decision.** The exact
invocations, recovered from each build tree's own `config.status` and
`config.log`, are in `RESUME.md` under *Prerequisites*. mogrix has package
rules for all four; building them through its pipeline instead needs `rpmbuild`,
which is what makes the staging tree reproducible.

### A Rust nightly, kept private

`env.sh` points `RUSTUP_HOME` and `CARGO_HOME` at
`ports/rust/{rustup,cargo}` rather than `~/.rustup` and `~/.cargo`. That is not
tidiness. Both mogrix steps rewrite shared state in place:

- `scripts/patch-rust-sysroot.sh` edits nightly's `std` source
- `mogrix patch-crates` edits crate sources inside the cargo registry

Neither is namespaced by target, so a second IRIX effort on the same machine
would silently rewrite this one's toolchain. Set up the private homes by copying
a nightly in, then running both patchers:

```sh
mkdir -p ports/rust/rustup/toolchains
cp -a ~/.rustup/toolchains/nightly-x86_64-unknown-linux-gnu \
      ports/rust/rustup/toolchains/
cp ~/.rustup/settings.toml ports/rust/rustup/

. ports/rust/env.sh
bash ~/repos/mogrix/scripts/patch-rust-sysroot.sh
uv run --directory ~/repos/mogrix mogrix patch-crates
```

Tested against `1.99.0-nightly (1ed2df61a 2026-08-04)`. The sysroot patcher
tracks a moving target: `std` reorganises, and a pattern that no longer matches
prints *"already patched or no match"*, which reads like success. If a build
fails somewhere inside `std`, suspect the patcher before the code.

### The compat archive

`build-compat.sh` builds `librust_irix_compat.a` from mogrix's compat sources —
the C stubs for what IRIX libc lacks (`dirfd`, `fdopendir`, `preadv`, `pwritev`,
`dup3`, `posix_fadvise`, `posix_fallocate`, `cfmakeraw`, `flock`, and
`__errno_location` for an errno that lives behind `__oserror()`).

It is a build artifact, so it is not in git, and
`ports/rust/agent-portable/compat` is a symlink to `ports/rust/hello/compat`.
**A fresh clone therefore has a dangling symlink until you run the script.**
Skipping it fails the link on an unresolved `-lrust_irix_compat`, which names
the archive but not the reason.

## What comes out

| binary | what it is |
|---|---|
| `rustdesk-agent` | the agent, built from the PPC tree's `main.rs` |
| `testpeer` | a client that speaks the real protocol; how to test without RustDesk |
| `capture-selftest` | the capture module through its Rust API, all paths |
| `pipeline` | capture → scale → convert → encode, with per-stage timings |
| `portable-selftest` | json, convert, png, zstd, crypto, config, http, protobuf |

`hello/` is a minimal `std` smoke test, useful when the toolchain itself is
suspect.

## Running it on an IRIX machine

The binary is a normal MIPS N32 executable and needs these at run time:

```
libX11.so.1  libXext.so  libc.so.1  libm.so  libpthread.so  libz.so   ← stock IRIX
libgcc_s.so.1                                                          ← NOT on IRIX
```

`libgcc_s.so.1` supplies the `_Unwind_*` symbols `std` references even under
`panic=abort`. It is in the staging tree at
`/opt/sgug-staging/usr/sgug/lib32/libgcc_s.so.1`.

**The binary carries an rpath of `/usr/lib/rustdesk-agent`**, which is where the
package puts a copy — so an installed agent needs no environment variable and no
wrapper. Running one straight out of `target/` on a development machine, where
nothing is installed, still wants the old incantation:

```sh
LD_LIBRARYN32_PATH=/usr/sgug/lib32 rustdesk-agent --password <PASSWORD>
```

## Making something installable

```sh
scripts/release.sh            # build -> gendist in the guest -> .tardist + .tar.gz
scripts/release.sh --no-inst  # no guest available: binaries and the tarball only
```

`docs/PACKAGING.md` is the whole pipeline: what goes in the package, why
`libgcc_s.so.1` is the only library in it, and the three different channels the
guest steps use.

`scripts/iris-install-test.sh --boot` installs the result in the emulator and
runs it with `LD_LIBRARYN32_PATH` deliberately unset, which is the only way to
find out that the rpath actually took. It is **not** part of a release — it
doubles a run — so run it when the packaging changes rather than every time.

## LLD cannot link SGI's static archives

Worth knowing before reaching for any other SGI-only library. IRIX ships XTEST
as `libXtst.a` and nothing else, and LLD refuses it twice over:

```
ld.lld-irix: error: libXtst.a(XTest.o): invalid sh_info in symbol table
ld.lld-irix: error: ...: multiple relocation sections to one section
                    are not supported
```

The first is a genuine defect in SGI's objects — `sh_info` on `.symtab` is 0,
where the spec says it is the index of the first non-local symbol — and
`tools/fix-sgi-archive.py` repairs it. The second is a real gap in LLD's MIPS
support that no post-processing fixes.

So `src/input_shim.c` issues the XTEST protocol requests itself. `XTestFakeInput`
is one request with a fixed 36-byte body, `Xlibint.h` is in the sysroot, and the
major opcode comes from `XQueryExtension`. Every X library that ships *shared*
links fine; this only bites on the extensions SGI shipped static.

## Testing what you built

Against a real or emulated IRIX machine with X running:

```sh
rustdesk-agent --password hunter2         # once
rustdesk-agent --listen 127.0.0.1 --port 21118 &
testpeer 127.0.0.1:21118 hunter2 120
```

`testpeer` prints frames, bytes and keyframes, and says plainly whether video
arrived. For the capture path on its own:

```sh
capture-selftest          # all three paths, band mapping, rebuild-after-drop
pipeline 4 2              # timings at 1/4 and 1/2 scale
rustdesk-agent --probe-display    # the agent's own view of the framebuffer
rustdesk-agent --probe-live       # mouse injection self-test
```

`probes/` holds the C probes that established the platform's behaviour in the
first place; each one's header says what question it answers.

## Emulator notes

`RESUME.md` has the full operating guide. The two that cost the most time:

- **`iris-ci start` is required** under `--ci` — the CPU thread is created
  paused, and a machine that has not been started is indistinguishable from a
  slow boot.
- **`iris` needs `--features chd`** to open the disk image, and the feature does
  not appear in its `build features:` banner, so the banner cannot tell you
  whether you have it. `hinv` on the guest is how you check which CPU you are
  running.
