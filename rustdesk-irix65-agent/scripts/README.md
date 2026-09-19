# scripts/ — the build and packaging pipeline

Thin on purpose. Every step is a script that runs the same way on a developer
machine and on a CI runner, and `../.github/workflows/irix-agent-build.yaml` is one
line per step — the only things in that YAML are the things only Actions can do.
The shape is `../irixscsitb`'s, including the variable names, so one licensed
image and one repository secret serve both projects.

## The pipeline

| | | guest |
|---|---|---|
| `build.sh` | cross-build the agent (MIPS III, and MIPS IV when the toolchain has it; `--isa`), the panel; stage the install tree; fail if the MIPS III agent holds a MIPS IV instruction | no |
| `iris-gendist.sh` | run `gendist` in IRIX — the only step Linux cannot do | yes |
| `package.sh` | the trio → `.tardist`, plus `.tar.gz` and checksums | no |
| `release.sh` | all of the above, in order | with `--boot` |

```sh
scripts/release.sh --boot        # cold: boots a guest, disposes of it
scripts/release.sh               # warm: attaches to a running one
scripts/release.sh --no-inst     # no guest at all: binaries + tarball
scripts/release.sh --boot --require-gendist   # what CI runs: no .tardist is a failure
```

`iris-install-test.sh` installs the package in a guest and runs what came out.
It is **not part of a release** — it doubles a run, nearly all of it inside
`inst` — and is opt-in everywhere. Run it when the PACKAGING changes; that is
the only kind of change it has ever caught anything on.

```sh
scripts/iris-install-test.sh --boot [--tarball] [--remove]
```

## Getting a machine to run on

| | |
|---|---|
| `fetch-image.sh` | resolve the IRIX boot image: `$IRIX65_IMAGE`, `ci/local.conf`, or `$IRIX65_DISK_URL` — a private URL, a secret in CI. `--check-only` preflights it; `--cache-key` gives `actions/cache` a key that moves with the image rather than with the commit. |
| `fetch-iris.sh` | the emulator: a local build with the `chd` feature wins, otherwise the prebuilt CLI from a `techomancer/iris` release. `--prebuilt` skips local builds. |
| `iris-guest.sh` | boot one and dispose of it. Never writes to the image; takes no port forwards; headless unless `--graphics` (offscreen REX3, no host display) or `--window`; `--cpu r4400` for MIPS III. |

## Building anywhere, from the image

| | |
|---|---|
| `ensure-rbcli.sh` | `rb-cli`, from PATH or a `danifunker/rusty-backup` release. Reads the image's XFS root with no emulator. |
| `make-sysroot.sh` | the n32 sysroot, out of the image, into `build/irix-sysroot`, in about fifteen seconds. **Licensed** -- never cached, uploaded or committed. |
| `toolchain.sh` | everything else the cross build needs, from pinned sources, into `build/toolchain`: mogrix, clang/LLD, the runtime objects, five static libraries (twice: MIPS III into `sgug`, MIPS IV into `sgug-mips4`), the patched nightly and registry. `--key` is its cache key; `IRIX_TOOLCHAIN` points a build at it. Reproduces the development machine's `/opt` toolchain (see `BUILD.md`). |

## The rest

| | |
|---|---|
| `ci-lib.sh` | sourced by all of them. Config resolution, version stamping, the inst product templating, and `guest_run` — which is how anything automated talks to the guest, over the serial console. |
| `install.sh` | ships **inside the tarball**. Installs by hand -- the MIPS IV agent on an R5000 or later, by `hinv` -- honours a different prefix, `-u` removes. Not part of the pipeline; part of the product. |

## Three rules that are not style

**Talk to the guest over the serial console.** The telnet forward stalls after a
few dozen sessions in a way that is indistinguishable from a wedged guest
(`docs/ISSUE-nat-inbound-stall.md`). `guest_run` uses `iris-ci run --shell sh`:
`--shell sh` because the guest's root shell is bash and iris-ci otherwise asks
csh for `$status`, so every exit code is a lie — and **one short command per
line**, because a long one comes back as several hundred bytes of garbage and a
syntax error on something nobody typed. Anything long goes in a script that the
guest fetches over HTTP.

**Set the cleanup trap before you start anything.** Both guest steps do, and
both use one handler rather than two `trap ... EXIT` lines, because the second
silently replaces the first. A packaging run that fails and leaves an emulator
behind is a machine somebody has to go and tidy; on a CI runner it is one that
never comes back.

**Readiness is "a command runs", not "a banner appeared."** `iris-ci boot`
waits for a console login prompt this image never presents the way it expects,
and the prompt prints five minutes after the guest is actually usable anyway.
`iris-guest.sh` polls until a command comes back.

`../docs/PACKAGING.md` is the full account, including what the install test is
worth on the occasions it is worth running.
