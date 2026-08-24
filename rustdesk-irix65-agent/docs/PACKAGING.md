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
configured the other. `--cache-key` prints a stable hash of the URL for
`actions/cache`, or the literal `local` so a caller can skip caching; a fetch
into an existing `--dest` is a no-op, which is what makes the cache a
transparent win.

`scripts/fetch-iris.sh` does the same for the emulator: a local build wins,
otherwise the prebuilt CLI from an iris release. Building iris from source is a
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
  and REX3 is where the emulator's remaining X wedges live. `--graphics` maps
  it for anything that drives the panel.

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
/usr/sbin/rustdesk-agent                    the agent
/usr/sbin/rustdesk-agent-gui                the Motif settings panel
/usr/lib/rustdesk-agent/agent-helper.sh     everything that decides anything
/usr/lib/rustdesk-agent/libgcc_s.so.1       the one library IRIX does not ship
/usr/lib/X11/app-chests/RustDesk.chest      the Toolchest entry
```

The agent links **libsodium, libvpx, mbedTLS and zstd statically**, so none of
them are here. What is left is what IRIX 6.5 already has — `libX11`, `libXext`,
`libz`, `libpthread`, `libm`, `libc` — and `libgcc_s.so.1`, which it does not.

`libgcc_s.so.1` is the interesting one. It is 117 KB, it comes from the cross
toolchain, and without it the agent does not start. Three ways to deal with that
and only one of them is any good:

- **An environment variable.** What every script in `ports/iris-run/` does:
  `LD_LIBRARYN32_PATH=/usr/sgug/lib32`. Fine for a development machine that has
  SGUG-RSE installed, useless as an installed product.
- **`/usr/lib32/libgcc_s.so.1`.** Dropping a GCC runtime into a system directory
  where some unrelated program will find it in a year's time.
- **An rpath.** The binary is linked with
  `-Wl,-rpath,/usr/lib/rustdesk-agent`, the package puts a copy there, and it
  simply works with no environment and no wrapper. This is what it does.

`scripts/iris-install-test.sh` runs the installed agent with
`LD_LIBRARYN32_PATH` explicitly **unset**, because every other script here sets
it and an rpath that quietly did nothing would never show up.

### What the install test is for, when you do run it

It found a bug that cannot exist anywhere else. `agent-helper.sh` decides
whether the agent is running with

```sh
ps -e -o pid,args | grep rustdesk-agent
```

which is correct in `/tmp` and wrong the moment the software is installed,
because the helper then lives in `/usr/lib/`**`rustdesk-agent`**`/agent-helper.sh`
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
the guest. `inst/rustdesk-agent.spec` and `inst/rustdesk-agent.idb` are
templates; `stage_inst_inputs` in `scripts/ci-lib.sh` stamps the version and
the ABI into them, and the whole staged tree goes into the guest as one tar.

The idb must be **sorted by destination path** or gendist rejects it. There is a
check for that in the repository:

```sh
LC_ALL=C sort -k5,5 -c inst/rustdesk-agent.idb
```

The numeric inst version is the first ten digits of the release version, which
begins with `YYYYMMDD` — so inst's own numeric comparison orders releases by
date without anything else having to care.

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
gunzip -c rustdesk-agent-VERSION-n32.tar.gz | tar xf -
cd rustdesk-agent-VERSION-n32 && sh install.sh
```

`install.sh` does the same thing by hand, honours a different prefix with `-p`,
and `-u` removes what it put there.

A non-default prefix breaks two lookups that are compiled in — the agent's rpath
names `/usr/lib/rustdesk-agent`, and the panel searches beside `argv[0]` and
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
- **Publishing.** There is no `publish-release.sh` here. irixscsitb's takes the
  artifact set and makes a GitHub release out of it; nothing about this
  pipeline's output would make that hard.

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
