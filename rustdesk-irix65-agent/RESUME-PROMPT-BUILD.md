# Resume prompt: a hosted build pipeline, the way ../irixscsitb does it

## Status after 2026-09-18 -- read this first

Priorities 1 to 3 below are **done**, and 4 is **built and rehearsed locally,
never run on GitHub**. `RESUME.md` §A HOSTED BUILD PIPELINE is the account;
this prompt is kept for its goal, pattern and boundaries, which still hold.

What is left, in order:

1. **Switched on, 2026-09-18** -- the workflow is committed at
   `.github/workflows/irix-agent-build.yaml`, filtered to `vintage-agents` and
   to the two agent trees. What it still needs is the `IRIX65_DISK_URL` secret
   **on `danifunker/rustdesk`**: when this was written it existed only on
   `danifunker/irixscsitb`, and secrets do not cross repositories. Check with
   `gh api repos/danifunker/rustdesk/actions/secrets` (names only).
2. **Watch the first run.** It is the first toolchain cache miss, the first
   time rustup 1.28 renames a dated nightly to `nightly` (verified only with
   1.26), and the first guest boot on a runner. If it goes red, the `logs`
   artifact has the console, iris and toolchain logs.
3. **Compare its artifact with a local build** of the same commit: with
   `IRIX_TOOLCHAIN` set locally they should be byte-identical; against the
   `/opt` toolchain, identical once `.symtab` is set aside.
4. Carry the corrected libvpx patch into mogrix (its untracked copy is stale)
   and track `compat/runtime/soft_float_stubs.c` there, so
   `ports/toolchain/` can go.

Everything below this section is the original prompt.

Continue the IRIX agent — shipped as **R-DeskVint**, binary `r-deskvint-irix`,
inst product `r_deskvint_irix`. Read `RESUME.md` first; it is the source of
truth for state, environment and the mistakes not to repeat. Do not re-derive
anything it records.

## The goal

A **build** pipeline, not a release pipeline: every push to `vintage-agents`
cross-compiles the agent, packages it — `.tar.gz` **and** a `.tardist` made by
`gendist` inside an IRIX 6.5 guest under iris — and leaves the result as run
artifacts. **No GitHub release is created.** A broken commit should show up as a
red run on that commit, not as a bad release.

It runs on a **hosted `ubuntu-latest` runner**, as irixscsitb's does. There is
no self-hosted runner and this work must not assume one.

**IRIX 6.5 / n32 only.** Not a preference: this agent is n32, and IRIX 5.3 has
no n32 ABI at all — it is o32-only. An o32 build would need
`~/repos/rust-irixlibstd`, a separate effort. Do not start it.

## The pattern to copy

`../irixscsitb/.github/workflows/release.yml` is written to be read as a sample,
and its header is the spec. The parts that matter here:

- **Thin YAML.** Every step body is one call into `scripts/`, and the same
  scripts run locally (`scripts/release-local.sh`). One code path. YAML holds
  only what only Actions can do: runner, secrets→env, cache, artifacts.
- **Hosted, because everything licensed lives in one private file.** The IRIX
  disk image comes from a secret URL (`IRIX65_DISK_URL`) or a local-path input,
  via `scripts/fetch-image.sh`. Nothing licensed is on the runner otherwise.
- **Prebuilt iris**, from `techomancer/iris` releases via `scripts/fetch-iris.sh`,
  pinnable by tag.
- **Build vs release**: a push to main builds and packages and stops at
  artifacts; only a tag push or a manual dispatch publishes. That is the half
  we want.
- **`--require-gendist`**: a guest that cannot build its product *fails the
  job*, rather than quietly shipping raw binaries.
- **Disposable guest**: `--fresh`, the image is never written (copy-on-write
  overlay), and the image download is cached (`actions/cache`, keyed by
  `fetch-image.sh --cache-key`).
- **The whole `dist/` tree travels between jobs.** Uploading only the binaries
  is exactly how irixscsitb once shipped releases with no `.tardist`.
- A **preflight** job fails in seconds, with the fix in the message, when the
  image source is missing.

## The one difference that matters, and how to close it

irixscsitb compiles C **natively inside the guest**, with the IRIX compiler that
is on the disk image. So its runner needs iris and the image and nothing else.

We **cross-compile Rust on the host**. The host needs `/opt/cross` (clang-18 +
`ld.lld-irix`), `/opt/sgug-staging` (the five static C libraries and
`libgcc_s`), a pinned patched nightly, and `/opt/irix-sysroot`. Of those, **only
the sysroot is licensed** — it is headers and libraries from an IRIX install.
The rest is built from open source by mogrix or downloaded.

So the problem reduces to getting one sysroot onto a hosted runner, and
irixscsitb already has the tool: `../irixscsitb/scripts/make-irix-sysroot.sh`
builds a sysroot tarball **from the IRIX disk image**, and takes `--abi n32`.
The runner downloads the image anyway, for gendist. So the design to try is:

    fetch image  ->  extract the n32 sysroot from it  ->  restore the rest of
    the toolchain from cache  ->  cross-compile  ->  boot a guest  ->  gendist
    ->  upload dist/

Everything licensed then comes from the one privately-hosted file, which is the
irixscsitb model exactly.

**This is a hypothesis, not a finding.** Verify first that
`make-irix-sysroot.sh --abi n32`, run against `~/Indy-IRIX65_dev.chd`, produces
something the build accepts in place of `/opt/irix-sysroot` — diff it against
the existing one on this machine. Note its warning: the direct mode needs a
case-sensitive host, and its robust mode tars inside the guest to keep IRIX
symlinks like `libc.so -> libc.so.1`.

`ci/workflows/irix-agent-release.yaml` currently says the toolchain "cannot be
downloaded on a hosted runner. That is what makes this self-hosted." That was
written before anyone looked at how irixscsitb does it, and it is what this work
should overturn. Correct that comment once hosted works.

Building clang-18 and the mogrix cross toolchain on every run is far too slow.
Cache the non-licensed parts (keyed on the mogrix commit and toolchain
versions), or build them once and publish them as an artifact. **Decide
deliberately where the sysroot may be cached** — `actions/cache` is scoped to the
repository, but this is a public fork; check what that means before relying on
it, and never let licensed material into anything publicly downloadable.

## Known state, from 2026-09-18

- **The local iris cannot boot the image.** `~/repos/iris/target/release/iris`
  was built without the `chd` feature: *"CHD image support not compiled in
  (rebuild with --features chd)"*. **Do not rebuild it** — `~/repos/iris` belongs
  to another session. Use a prebuilt release instead.
- **Our `fetch-iris.sh` points at a dead repo.** It defaults to `danifunker/iris`,
  whose releases API answers **404**; irixscsitb moved to `techomancer/iris`,
  which answers 200 and publishes `IRIS-cli-linux-x64-*.tar.gz` (latest seen:
  `v2026-08-31-16-28`). Change the default. Both checked 2026-09-18.
- **techomancer's release also publishes `Indy-IRIX65_dev.chd`** as an asset. Do
  not assume that is fine to depend on for licensed material — this repo's docs
  say to host the image privately. It is a decision, not a default.
- **The agent binaries are already MIPS III.** The target spec sets
  `"cpu": "mips3"` and `file` says `MIPS-III`. iris emulates an `r4400` (MIPS III,
  its default) or an `r5000` (MIPS IV). The only real hardware so far is the O2,
  an R10000 — MIPS IV — so **the agent has never actually run on a MIPS III CPU.**
  An r4400 guest is the first place that can happen.
- **The O2 can package, but it is not the pipeline.** `gendist` on `sgio2`
  (192.168.99.41) produced a `.tardist` that installs with `inst` and registers
  as `r_deskvint_irix` (commit `db5ccb9ff`). That proved the product definition.
  CI must not depend on a machine on the LAN.
- `scripts/iris-gendist.sh --boot` already does the disposable-guest gendist.
  Reuse it; do not write a second one.
- The inst version is stamped by `dist_version_from` in `scripts/ci-lib.sh`,
  truncated to ten digits. A build must stamp something deterministic.
- The Rust toolchain was lost and rebuilt on 2026-09-17; `RESUME.md` §Rust
  cross-compilation, "Rebuilt from nothing", is the exact recipe. Locally,
  **protect the shared `~/.rustup` and `~/.cargo`** — both mogrix patchers rewrite
  in place and are not namespaced by target.

## Priorities, in order

1. **Fix `fetch-iris.sh`** to default to `techomancer/iris`, and confirm the
   prebuilt iris boots `~/Indy-IRIX65_dev.chd`. This alone unblocks every
   guest-based step locally.
2. **Make `scripts/iris-gendist.sh --boot` produce a `.tardist` end to end**, then
   install-test it in the guest with `scripts/iris-install-test.sh`.
3. **Run the agent on MIPS III** in an r4400 guest: `--show-id`, the helper's
   `status`, and `--probe-display` against the guest's X server. First run on
   that ISA ever; record what it measures.
4. **Only then, the hosted pipeline**: sysroot from the image, toolchain caching,
   a thin build workflow with build-not-release semantics, preflight.

## Boundaries

- Do not modify `~/repos/iris`.
- Never commit, cache publicly, or upload licensed material — the sysroot, the
  disk image, anything extracted from either.
- Build only. No publish step, no GitHub release.
- The repository root's `.github/workflows/` is upstream RustDesk's CI (and
  R-DeskWay's on the wayland branch). Our workflow sits inactive in
  `ci/workflows/` on purpose. If you activate one, filter it to `vintage-agents`
  so it never fires on an upstream-tracking branch, and say so plainly.
- Do not change the O2's configuration. `grabServer` is `False` there by
  deliberate choice; the original is at
  `/usr/lib/X11/xdm/xdm-config.before-rustdesk`.

When you finish or run out of road, update `RESUME.md` in place and give a
summary that says plainly what worked, what did not, and what you are unsure
about. An accurate description of a failure is worth more than a hopeful one.
