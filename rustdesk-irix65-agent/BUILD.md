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

## The short way: any Linux host and the disk image

Since 2026-09-18 everything the build needs can be made from the IRIX disk
image plus pinned open-source inputs, with no `/opt` and nothing copied from
another machine. This is what CI does, and it works the same locally:

```sh
sudo apt-get install clang-18 llvm-18 python3-yaml    # Ubuntu 24.04
export IRIX65_IMAGE=~/Indy-IRIX65_dev.chd              # or IRIX65_DISK_URL
scripts/make-sysroot.sh     # SGI's headers and libraries, out of the image: ~15 s
scripts/toolchain.sh        # everything else, from source, into build/toolchain
export IRIX_TOOLCHAIN=$PWD/build/toolchain IRIX_SYSROOT=$PWD/build/irix-sysroot
scripts/build.sh            # the agent and the panel, staged for packaging
```

`make-sysroot.sh` extracts the n32 sysroot with `rb-cli` and adds the two things
a bare extraction misses (the Motif headers `/usr/include/Xm` links to, and
mogrix's stripped `crt1.o`/`crtn.o`). `toolchain.sh` fetches mogrix at a pinned
commit (which carries the patched `ld.lld-irix-18` as a binary), builds its
runtime objects and the five static libraries below from checksummed tarballs,
and installs and patches the pinned nightly -- all inside `build/toolchain`,
never touching `~/.rustup` or `~/.cargo`.

**It was checked against this machine's hand-built `/opt`,** object by object:
every runtime object, `libgcc_s`, zlib, libsodium and all three mbedTLS
archives came out byte-identical; zstd differs in one object, by the build path
an `assert` records; libvpx is identical in all 83 objects apart from ELF
`FILE` symbols the original archive lacks. The agent built through either
toolchain is identical in everything that loads -- only `.symtab` differs, by
those symbols -- and the panel is identical outright. Getting libvpx to match
found that `patches/libvpx-vp8-active-map-early-out.patch` did not describe the
libvpx that had actually shipped; the patch now does. See `RESUME.md`.

The sysroot is licensed material. It lives in `build/`, is never committed or
cached, and takes fifteen seconds to remake.

---

## The build needs no emulator

This is a cross-compile: clang-18 plus `ld.lld-irix` targeting
`mips-sgi-irix6.5`. The timings above were measured with no emulator running at
all. `iris` is only needed to *run* what comes out, and on current evidence a
real SGI would be a better place to run it — see the blocker in the README.

What the build *does* need from an IRIX machine is a sysroot: headers and
shared libraries from a 6.5.22m installation. On this machine that is
`/opt/irix-sysroot`, pulled off the guest by hand; anywhere else,
`scripts/make-sysroot.sh` takes the same files out of the disk image, and the
two produce identical binaries.

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

### Five C libraries, in the staging tree

All statically linked into the agent, so nothing has to be installed on the
target machine:

| | notes |
|---|---|
| **libsodium 1.0.18** | no source patches. Consumers must link `-lpthread` or `sodium_init()` returns −1 while every `crypto_*` call still works |
| **libvpx 1.13.1** | one patch; VP8 only, `--disable-webm-io --disable-libyuv` (both are C++ and drag in the host linker) |
| **mbedTLS 3.6.2** | two patches |
| **zstd 1.5.6** | no patches; `clipboard.rs` links it |
| **zlib 1.3.2** | no patches; `png.rs` links it. `CC=irix-cc ./configure --static && make libz.a`, then copy `libz.a` into `$SGUG_STAGING/lib32`. **Do not** fall back to the machine's own zlib — see below |

`zlib` is the newest of these and the only one that was ever linked
dynamically. It is static now because IRIX 6.5 shipped more than one zlib and
the older one predates `compressBound`, which made the agent die on a stock O2
while working on the build image. `docs/PACKAGING.md` has the whole account; the
short version is that a machine's own zlib cannot be relied on.

**On this machine they were built by hand into `/opt/sgug-staging`, outside
every git tree.** `scripts/toolchain.sh` now builds all five from pinned,
checksummed sources with the same flags, and reproduces those hand-built
archives (see *The short way*, above) -- so the recipes are in git at last, as a
script rather than as notes. mogrix also has package rules for all of them;
building through its pipeline needs `rpmbuild`.

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
| `r-deskvint-irix` | the agent, built from the PPC tree's `main.rs` |
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

**The binary carries an rpath of `/usr/local/lib/r-deskvint-irix`**, which is where the
package puts a copy — so an installed agent needs no environment variable and no
wrapper. Running one straight out of `target/` on a development machine, where
nothing is installed, still wants the old incantation:

```sh
LD_LIBRARYN32_PATH=/usr/sgug/lib32 r-deskvint-irix --password <PASSWORD>
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
r-deskvint-irix --password hunter2         # once
r-deskvint-irix --listen 127.0.0.1 --port 21118 &
testpeer 127.0.0.1:21118 hunter2 120
```

`testpeer` prints frames, bytes and keyframes, and says plainly whether video
arrived. For the capture path on its own:

```sh
capture-selftest          # all three paths, band mapping, rebuild-after-drop
pipeline 4 2              # timings at 1/4 and 1/2 scale
r-deskvint-irix --probe-display    # the agent's own view of the framebuffer
r-deskvint-irix --probe-live       # mouse injection self-test
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
