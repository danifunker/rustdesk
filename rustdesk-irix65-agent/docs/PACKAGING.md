# Packaging: how a build becomes something you can install

The agent used to run from `/tmp`. This is how it becomes a product that
installs into an IRIX machine the ordinary way — double-click a `.tardist`, or
`inst -f` — and runs on a machine that has never had a cross toolchain, a
Rust nightly or SGUG-RSE anywhere near it.

The shape is taken from [`../irixscsitb`](../../irixscsitb): an `inst/` product
description, a `desktop/` Toolchest fragment, and `scripts/` that are thin
enough that a CI job would be a one-line call into each. The mechanics are
identical because IRIX only offers one way to do this; the differences are
noted where they exist.

## The pipeline

```
scripts/build.sh          cross-build + stage        Linux, ~55 s, no emulator
scripts/iris-gendist.sh   gendist                    needs a running IRIX guest
scripts/package.sh        .tardist + .tar.gz         Linux, seconds
scripts/iris-install-test.sh   install it and run it needs a running IRIX guest

scripts/release.sh        all three, in order
```

Each step leaves its output on disk and can be re-run alone, which matters
because the one that fails is nearly always the middle one — it is the only one
that needs a live IRIX.

```sh
scripts/release.sh --boot          # cold: boots its own guest, disposes of it
scripts/release.sh                 # warm: attaches to a running one
scripts/release.sh --no-inst       # no guest at all: binaries + tarball
scripts/iris-gendist.sh --boot     # just re-run the packaging step
```

**The install test is not part of a release.** `scripts/iris-install-test.sh`
roughly doubles a run — ten minutes of twenty, nearly all of it inside `inst` on
an emulated R5000 — and shipping a package should not cost that every time. It
is opt-in everywhere: `release.sh --install-test`, or the `install_test` input
in the workflow, both off by default.

Run it when the **packaging** changes: the file list in the idb, an install
path, what the helper or the panel looks for. That is the only kind of change it
has ever caught anything on — see below, where it caught something invisible
anywhere else. For an ordinary code change it tells you nothing new, and the
trade is deliberate: a release that is quick, and a class of install-only bug
that will get through until someone runs it.

```sh
scripts/iris-install-test.sh --boot                      # does it install and run
scripts/iris-install-test.sh --boot --tarball --remove   # and the other two paths
```

**`--boot` is the difference between a script and a pipeline.** Without it, both
emulator steps attach to a guest that somebody started by hand — which is what
you want while iterating, and is not something CI can do. With it they call
`scripts/iris-guest.sh`, which resolves an image, starts a guest, waits for the
login prompt, and takes it away again on the way out however the run ends.
`release.sh --boot` starts **one** guest and both steps share it, because a boot
is five minutes on an emulated R5000.

`--tarball` installs the `.tar.gz` with `install.sh` under `/opt/rdtest` — a
non-default prefix, which is the path in `install.sh` that writes wrappers and
therefore the one most likely to have rotted. `--remove` runs
`versions remove`. Neither is on by default because each adds minutes to a run
that is already dominated by how long `inst` takes on an emulated R5000.

Per-machine paths live in `ci/local.conf` (copy `ci/local.conf.example`). It is
parsed as `KEY=VALUE` and never sourced, and anything already in the environment
wins over it.

## Where the image comes from

The IRIX boot image is a licensed install, so it is never in the repository and
never in a public bucket. `scripts/fetch-image.sh` resolves it, first match
wins:

| | |
|---|---|
| `$IRIX65_IMAGE` | a local path. In Actions this is the `irix65_image` dispatch input, which is how a self-hosted runner uses an image that never leaves the machine. |
| `ci/local.conf` | the same key, per-machine, `.gitignore`'d. |
| `$IRIX65_DISK_URL` | a private download URL — **a repository secret**. A bare `.chd` or a `.zip` containing one. |

Those are `../irixscsitb`'s names, deliberately: the same image and the same
secret serve both repositories, and someone who has configured one has
configured the other. `--cache-key` prints a stable hash of the URL, or the
literal `local`; a fetch into an existing `--dest` is a no-op.

**The image is also where the sysroot comes from.** `scripts/make-sysroot.sh`
takes SGI's headers and libraries out of it with `rb-cli` in about fifteen
seconds, and the agent built against that is identical to one built against the
sysroot this project had always used. So the image is the only licensed input
anywhere in the pipeline, and a hosted runner needs nothing else private.

**Neither the image nor the sysroot is cached in CI**, and that is a decision,
not an oversight. This fork is public, and GitHub's documentation says of
`actions/cache`: *"Anyone with read access can create a pull request on a
repository and access the contents of a cache."* On a public repository that is
everyone. `../irixscsitb` caches its images; this workflow downloads the image
every run instead and caches only the toolchain, which holds nothing of SGI's
(`scripts/toolchain.sh`'s header says why). `--cache-key` is still used -- to key
the toolchain cache on the image its libraries were compiled against.

`scripts/fetch-iris.sh` does the same for the emulator: a local build wins,
otherwise the prebuilt CLI from an iris release -- **`techomancer/iris`**, the
upstream emulator, since 2026-09-18. The old default, `danifunker/iris`,
publishes no releases any more (its API answers 404). A local build that was
compiled without the `chd` feature is skipped with a message rather than used:
it would boot, read its config, and then refuse the disk. `--prebuilt` ignores
local builds altogether, which is what CI passes. Building iris from source is a
Rust toolchain plus clang and libclang for the chd feature, which turns a job
that should be seconds into minutes.

## The guest a run gets

`scripts/iris-guest.sh start` boots one and prints the shell assignments to
attach to it. Three properties, each of which was a decision:

- **It never writes to the image.** The generated config sets `overlay = true`,
  so every write goes to a `.diff.chd` sidecar; `stop` deletes it. A licensed
  install that a build has mutated is not something you can hand to a second
  machine — and in CI the image is downloaded fresh anyway, so anything written
  to it is lost silently rather than usefully. The image is **symlinked** into
  the work directory and iris is pointed at the link, so the sidecar lands
  beside the link rather than beside the original.
- **No port forwards.** The packaging steps need none: commands go over the
  serial console, files go in over HTTP — the guest dialling out through NAT,
  not a forward — and come out through `iris-ci get`. With no host ports there
  is nothing to collide over, so two runs on one machine do not fight and
  neither can steal the telnet port from a developer's own emulator. The
  control socket is per-run for the same reason: iris **deletes and rebinds**
  whatever socket path it is given.
- **Headless by default.** `gendist` and `inst` never touch the framebuffer,
  and REX3 is where the emulator's remaining X wedges live. `--graphics` gives
  the guest its REX3, rendered into an offscreen buffer -- Xsgi runs and the
  agent can capture, with no host window and no host X display, so it works on
  a CI runner. `--window` also shows it on `$DISPLAY`. `--cpu r4400` boots a
  MIPS III machine instead of the default R5000 (MIPS IV).

**A full build host panics the guest.** Everything the guest writes goes to a
copy-on-write overlay on the host; a write that cannot be satisfied is a fatal
error to IRIX — `PANIC: Fatal error on root filesystem`, a dump that also fails,
and nothing naming the real cause. `iris-guest.sh` checks `df` before booting
(2 GB floor, `IRIS_GUEST_MIN_MB` overrides) and watches for `PANIC:` while it
waits, because to a poll that only asks whether a command ran, a panic looks
exactly like a slow boot.

**`iris-ci get` can pick the wrong shell.** It probes `echo ZZSHELLZZ=$0` and
chooses sh or csh syntax from the answer; on a console that has just been worked
hard the probe misses and it sends `>& /dev/null` and `$status` to bash. The
transfer then fails reporting *"iris-ci get needs a shell on the serial
console"* — and the shell is right there. Once in about fifteen transfers, and
it costs the whole twenty-minute run. `guest_get` settles the console and
retries. `get` has no `--shell` flag the way `run` does; if it grows one, this
retry can go.

**Readiness is "a command runs", not "a banner appeared."** `iris-ci boot`
waits for the console login prompt and is the obvious call to make here — and
on this image it sits through its entire timeout while `IRIS console login:` is
already in `console.log`, because the PROM autoboots straight past the menu it
watches for. `iris-guest.sh` polls `iris-ci run 'echo GUEST-READY'` instead,
which asks the only question that matters.

It is also **much** faster, for a reason that was already written down.
`RESUME.md` has said since the first session that "telnetd answers well before
the serial console prints its login banner" — and so does the console shell.
The guest answered a command **110 seconds** in; the banner did not appear for
another five minutes. Waiting for it was throwing those five minutes away on
every run.

## What the package contains, and why so little

```
/usr/local/sbin/r-deskvint-irix                    the agent (one of two builds)
/usr/local/sbin/r-deskvint-irix-gui                the Motif settings panel
/usr/local/lib/r-deskvint-irix/agent-helper.sh     everything that decides anything
/usr/local/lib/r-deskvint-irix/libgcc_s.so.1       the one library IRIX does not ship
/usr/lib/X11/app-chests/R-DeskVint.chest      the Toolchest entry
```

**The agent comes in two builds, and a machine gets one.** Since 2026-09-18
the stage has `bin/r-deskvint-irix`, built for MIPS III -- every IRIX 6.5
machine runs it -- and `bin/r-deskvint-irix-mips4`, built for MIPS IV: R5000,
R8000, R10000 and later (R12000-R16000 are MIPS IV too; no SGI machine ever
shipped a MIPS V CPU). Both install to the same path. The idb gives the first
`mach(CPUARCH=R4000)` and the second `mach(CPUARCH=R5000 CPUARCH=R8000
CPUARCH=R10000)`, and inst installs whichever this machine's CPUARCH matches --
`/var/inst/machfile` maps the processor ID to those four names, and between
them they are every CPU 6.5 runs on. `install.sh` makes the same choice from
`hinv`, falling back to MIPS III for anything it does not recognise.
`iris-install-test.sh` checks inst's choice against the guest's CPU.

What MIPS IV buys is modest -- 3-6% on the encoder on an O2, where the frame is
the whole cost -- and it costs nothing at run time. A stage built for MIPS III
alone (`scripts/build.sh --isa mips3`, or a machine without the MIPS IV
staging tree) packages its one agent with no `mach()` tag at all.

**Two things are not in the package, and are not removed with it.**

- **`/etc/r-deskvint-irix.conf`**, all of the machine's settings and its
  identity (ID, uuid, keypair, password, servers). The agent writes it; root
  alone can read or change it. It is not an inst file on purpose: `versions
  remove` and `install.sh -u` leave it, so a reinstall is the same machine.
  Builds before 2026-09-18 kept it in root's `~/.rustdesk-ppc-agent.conf`, and
  the first thing run as root afterwards -- the boot start included -- moves it.
- **The boot start**, `/etc/init.d/r_deskvint_irix` and its rc links, made by
  `agent-helper.sh service install` and switched on with `chkconfig
  r_deskvint_irix on`. That copies the init script out of the package, so **an
  upgrade does not refresh it**: run `service install` again after one (it
  leaves the chkconfig flag as it was). The copy has so far only ever called
  the helper, which the upgrade does replace.

The agent links **libsodium, libvpx, mbedTLS, zstd and zlib statically**, so
none of them are here. What is left is what IRIX 6.5 already has — `libX11`,
`libXext`, `libpthread`, `libm`, `libc` — and `libgcc_s.so.1`, which it does not.

**zlib is in that static list for a reason found the hard way.** IRIX 6.5 does
ship zlib, so this list originally treated it as a system library and `build.rs`
linked it with `-lz`. But 6.5 shipped more than one zlib across its life: the
sysroot pulled from the 6.5.22m build image is 1.2.1 and has `compressBound`,
while a stock 6.5.22m O2 carries an older one that does not. A build linked
against the sysroot therefore started on the build image and died everywhere
else with

```
rld: Error: unresolvable symbol in /usr/local/sbin/r-deskvint-irix: compressBound
rld: Fatal Error: this executable has unresolvable symbols
```

which `rld` writes to **SYSLOG and not stderr**, so what you actually see is a
silent `exit 1` with no output at all — from `--show-id`, from `--help`, from
everything. `par -s -SS` on the process is what finds it; `/var/adm/SYSLOG` is
where it says so. Found on a real O2 on 2026-09-17, having passed every install
test until then (see below).

The fix is `cargo:rustc-link-lib=static=z` in `agent-portable/build.rs`, against
a zlib 1.3.2 built for n32 by `ports/work/zlib-1.3.2` and staged into
`$SGUG_STAGING/lib32/libz.a` — the same treatment libvpx and mbedTLS already
get. Shipping a `libz.so` beside the agent would also have worked, but it would
have meant carrying SGI's 2003-vintage 1.2.1, which is old enough to predate
the fixes for CVE-2018-25032 (a `deflate` bug, and `png.rs` is a deflate caller)
and CVE-2022-37434. Static linking removes the machine's zlib from the question
entirely.

`libgcc_s.so.1` is the interesting one. It is 117 KB, it comes from the cross
toolchain, and without it the agent does not start. Three ways to deal with that
and only one of them is any good:

- **An environment variable.** What every script in `ports/iris-run/` does:
  `LD_LIBRARYN32_PATH=/usr/sgug/lib32`. Fine for a development machine that has
  SGUG-RSE installed, useless as an installed product.
- **`/usr/lib32/libgcc_s.so.1`.** Dropping a GCC runtime into a system directory
  where some unrelated program will find it in a year's time.
- **An rpath.** The binary is linked with
  `-Wl,-rpath,/usr/local/lib/r-deskvint-irix`, the package puts a copy there, and it
  simply works with no environment and no wrapper. This is what it does.

`scripts/iris-install-test.sh` runs the installed agent with
`LD_LIBRARYN32_PATH` explicitly **unset**, because every other script here sets
it and an rpath that quietly did nothing would never show up.

**What that test cannot tell you.** It installs into the development guest, and
that image has `/usr/sgug` populated with SGUG-RSE — which supplies a modern
zlib. So the test proved the package installs on a machine that already had the
toolchain's libraries, which is the opposite of what it was written to prove. A
genuinely stock machine has no `/usr/sgug` at all. Until one was available the
claim in this file — that the package installs on an IRIX that has never heard
of SGUG-RSE — was never actually tested.

### What the install test is for, when you do run it

It found a bug that cannot exist anywhere else. `agent-helper.sh` decides
whether the agent is running with

```sh
ps -e -o pid,args | grep r-deskvint-irix
```

which is correct in `/tmp` and wrong the moment the software is installed,
because the helper then lives in `/usr/lib/`**`r-deskvint-irix`**`/agent-helper.sh`
— so the pattern matches the helper's own shell, `is_running` always answers
yes, and the panel's **Start button is greyed out for ever**. Nothing on the
build host can see that. Nothing in a `/tmp` test can see it. It appears exactly
when the software is put where it is meant to live, and it disables the
principal button.

The matcher is now the basename of `argv[0]` compared exactly. The general
lesson is smaller than the bug: a program that identifies its own processes by
grepping a command line will eventually match its own installation path.

The panel needs no rpath: `libXm`, `libXt`, `libXext`, `libX11`, `libm`, `libc`
are all stock. Motif 1.2.4 is what 6.5 ships.

## The part only IRIX can do

An inst product is three files — a spec, an idb, and a `.sw` archive in SGI's
own format — and `gendist`(1M) is the only thing that writes them. It runs in
the guest. `inst/r-deskvint-irix.spec` and `inst/r-deskvint-irix.idb` are
templates; `stage_inst_inputs` in `scripts/ci-lib.sh` stamps the version and
the ABI into them, and the whole staged tree goes into the guest as one tar.

The idb must be **sorted by destination path** or gendist rejects it. There is a
check for that in the repository:

```sh
LC_ALL=C sort -k5,5 -c inst/r-deskvint-irix.idb
```

The numeric inst version is ten digits: the release version's `YYYYMMDD`, then
how far through that UTC day the commit was made, 00-99 -- so inst's own
numeric comparison orders releases by commit time without anything else having
to care. Until 2026-09-18 the last two digits came from the revision hash, and
two commits on one day could order backwards: `cf5c43c05` was `2026091854`,
`ed74049` before it `2026091874`. (`dist_version_from` in `scripts/ci-lib.sh`.)

## Three channels to the guest, and why each is what it is

Everything about talking to the guest was rewritten during this work, because
the obvious channel is the one that fails.

| what | how | why not the other way |
|---|---|---|
| commands | **serial console**, `iris-ci run --shell sh` | the telnet forward stalls after a few dozen sessions: still accepted, nothing ever comes back. See `ISSUE-nat-inbound-stall.md`. The console has never failed. |
| host → guest | **HTTP**, the guest's `wget` to `192.168.0.1` | 10 MB down a serial line is not a thing you do twice. Outbound has never failed either. |
| guest → host | **`iris-ci get`**, console + scratch volume | the guest cannot push a file anywhere. This is the only way back. |

Two console rules that are not optional: `--shell sh`, because the guest's root
shell is bash and iris-ci defaults to asking csh for `$status` — so every
command returns "guest exit -1" and the exit code is a lie; and **one short
command per line**, because a long one comes back as several hundred bytes of
garbage and a syntax error on something nobody typed. Anything long goes into a
script that is fetched over HTTP.

## Installing, at the other end

```sh
# Software Manager, the normal way
inst -f /path/to/unpacked-tardist
#   install standard
#   go

# or, without inst
gunzip -c r-deskvint-irix-VERSION-n32.tar.gz | tar xf -
cd r-deskvint-irix-VERSION-n32 && sh install.sh
```

`install.sh` does the same thing by hand, honours a different prefix with `-p`,
and `-u` removes what it put there.

A non-default prefix breaks two lookups that are compiled in — the agent's rpath
names `/usr/local/lib/r-deskvint-irix`, and the panel searches beside `argv[0]` and
then that same directory for the helper. Both are bridged with a small wrapper
that sets a variable, because nothing should quietly rewrite a linked path
behind anyone's back. The helper needs no wrapper: it looks beside *itself*
first, for both the agent and the library, which costs one line and works under
any prefix.

## Where this could go next

Three things are shaped for it and not done:

- **Make `inst` cheaper, not the boot.** The obvious optimisation is snapshots
  — `iris-ci save` / `restore` exists and restore is quoted at ~145 ms — and
  fixing the readiness test took most of the prize away first: the boot is now
  about two minutes of a twenty-minute run. What dominates is `inst` itself, at
  roughly ten. A snapshot would still pay, and it would want an invalidation
  rule (a snapshot is tied to an image *and* an emulator build; `--cache-key`
  computes half of one) — but the honest measurement says look at the install
  test before the boot. Reusing one guest across a matrix, which `release.sh
  --boot` already does for two steps, is the same idea for less work.
- **An o32 flavor**, packaged by a 5.3 guest. `--abi` and `stage_inst_inputs`
  take it already; what is missing is an o32 build of the agent, which is the
  other effort in `~/repos/rust-irixlibstd`, and a 5.3 image. `../irixscsitb`
  runs exactly this as a two-flavor matrix and is the thing to copy.
- **Publishing.** There is no `publish-release.sh` here, deliberately:
  `.github/workflows/irix-agent-build.yaml` (at the repository root) builds and
  packages on every push and
  stops at artifacts. irixscsitb's takes the artifact set and makes a GitHub
  release out of it; nothing about this pipeline's output would make that
  hard, and it should be its own decision.

## What is deliberately not in the package

- **A desktop icon.** The Toolchest entry is there; a proper Indigo Magic icon
  is FTR rules plus a vector `.icon` file and a type-database rebuild. See
  `../irixscsitb/desktop/` for what that involves.
- **An init script.** The agent does not start at boot. `agent-helper.sh start`
  and the panel's Start button are how it runs today, and a service that comes
  up on its own before anyone has set a password is not obviously what a person
  wants.
- **An o32 flavor.** The scripts take `--abi` and `stage_inst_inputs` knows
  about o32, because irixscsitb's arrangement — each OS packages its own build
  — is the one worth keeping. Nothing has been built for o32.
