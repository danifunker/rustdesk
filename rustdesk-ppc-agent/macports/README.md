# MacPorts port, for macos-powerpc/powerpc-ports

The files here are laid out exactly as the ports tree expects, so preparing the
PR is a copy:

```sh
cp -r macports/net/rustdesk-ppc-agent  <powerpc-ports>/net/
cp    macports/lang/mrustc/files/0026-*.patch  <powerpc-ports>/lang/mrustc/files/
```

and one line added to `lang/mrustc/Portfile`, after the existing `0025`:

```tcl
patchfiles-append   0026-minicargo-Preserve-the-semver-pre-release-suffix-in-.patch
```

## What is here

| file | what it is |
|---|---|
| `net/rustdesk-ppc-agent/Portfile` | the port |
| `net/rustdesk-ppc-agent/files/protobuf-3.0.0-alpha.2.patch` | one crate fix, described below |
| `lang/mrustc/files/0026-*.patch` | a minicargo fix the port needs |

## Why mrustc needs a 26th patch

`protobuf` is pinned at `3.0.0-alpha.2`, to match the `.proto`-generated code
this agent shares with RustDesk. minicargo's `PackageVersion::from_string`
parses a semver `+build` suffix but not `-prerelease`, so `CARGO_PKG_VERSION`
reaches the build script as plain `3.0.0`: it emits `VERSION_3_0_0` while the
crate's own generated files reference `VERSION_3_0_0_ALPHA_2`, and the name
fails to resolve.

The fix is upstream-bound (a PR against thepowersgang/mrustc), but the port need
not wait for it: it applies cleanly on top of the existing `0001`-`0025`, right
above the `CARGO_PKG_*` block that `0012` adds. That was checked against the
patched tree, not assumed.

Bumping protobuf to 3.x stable would avoid the patch, but 3.x is edition 2021,
which this toolchain handles poorly. The alpha pin looks like the better trade.

## What has been verified, and on what

Everything below was built with `mrustc` at `d0fffe5b` plus all 25 ports
patches plus the `0026` above — that is, the port's exact compiler.

**Verified**

* The whole 39-unit crate graph builds. protobuf, sodiumoxide, sha2, libc, the
  five C shims, and the agent's own 36 MB of generated C.
* No layout `sizeof_assert` failures anywhere, including protobuf's
  `ReflectValueRef` — the type that needed a local mrustc patch before
  `0002` replaced the alignment model. `0002` subsumes it, as it claims.
* The resulting binary is a correct `Mach-O ppc_970 executable`.
* Only one crate needs patching (protobuf, above). No mrustc workarounds are
  needed in any other crate, and `libc` is pinned at `0.2.174`, before the
  `src/new/` tree — so this port does not need the `libc` patch that others do.

**Not verified — and a reviewer should assume these are open**

* **Never built natively.** mrustc ran on a Linux host emitting C, which was
  compiled on a real G5 over ssh. A MacPorts build compiles everything on the
  Mac. Nothing in the crate graph should care, but it has not been shown.
* **Never built or run on 10.6.** The hardware here is a G5 on **10.5.8**. The
  agent's capture path (`CGDisplayBaseAddress`) and cursor path
  (`CGSGetGlobalCursorData`) are deprecated-but-present in 10.6 and private,
  respectively; both degrade to a logged error rather than a crash.
* **Checksums are placeholders.** They need a real tagged release; see the TODO
  at the top of the Portfile.
* **`supported_archs ppc`** is a statement of what was built and tested, not a
  claim that ppc64 cannot work.

## Two things the port does *not* need, which our own build does

* **No `ppc-compat.c`.** Our cross build links a small shim supplying the libc
  entry points 10.4/10.5 lacks (`poll`, the `$INODE64` `stat` family,
  `realpath$DARWIN_EXTSN`, `lgammaf_r`). The mrustc PortGroup already adds
  `legacy-support` as a build dependency and puts
  `-Wl,${prefix}/lib/libMacportsLegacySupport.a` on the link line for any
  deployment target below 10.12, so the port gets the same coverage from
  MacPorts. Most of those symbols exist natively on 10.6 anyway.
* **No `-latomic`, and no `AtomicU64`.** The port's `0003` drops `AtomicU64` on
  ppc32, matching rustc, which sets `max_atomic_width` 32 on every 32-bit
  PowerPC target. The agent used one `AtomicU64` for a log timestamp; it now
  uses two `AtomicU32`s and needs no libatomic.

## `-fno-gcse` in `build.rs`, if a reviewer asks

The five C shims are compiled at `-O2` with `-fno-gcse`. That is not
superstition: MacPorts' `gcc10-bootstrap` for `powerpc-apple-darwin` generates a
faulting access for ordinary double arithmetic over file-scope statics at `-O2`,
and only at `-O2` — `-O0`, `-O1` and `-Os` are all fine, and of the passes `-O2`
adds, disabling global common subexpression elimination is the one that avoids
it. It cost a live session before it was found; the disassembly is in the
upstream `BACKLOG.md` item 1e.

It may well be unnecessary under whatever compiler the port selects on 10.6. It
is harmless there either way, and narrower than dropping the shims to `-O1`.

## The launchd question

The port installs a LaunchAgent *template* rather than using `startupitem`,
which would produce a LaunchDaemon. That is deliberate: `PasteboardCreate`
returns `-4960` outside the logged-in Aqua session, so a daemon-launched agent
serves a session whose clipboard silently does not work. The `notes` block tells
the user how to install it per-account.
