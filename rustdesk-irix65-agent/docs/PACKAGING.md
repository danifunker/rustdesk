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
scripts/release.sh                  # everything
scripts/release.sh --no-inst        # no guest? still get binaries + tarball
scripts/iris-gendist.sh             # just re-run the packaging step
scripts/iris-install-test.sh        # prove the package installs
scripts/iris-install-test.sh --tarball --remove   # and the other two paths
```

`--tarball` installs the `.tar.gz` with `install.sh` under `/opt/rdtest` — a
non-default prefix, which is the path in `install.sh` that writes wrappers and
therefore the one most likely to have rotted. `--remove` runs
`versions remove`. Neither is on by default because each adds minutes to a run
that is already dominated by how long `inst` takes on an emulated R5000.

Per-machine paths live in `ci/local.conf` (copy `ci/local.conf.example`). It is
parsed as `KEY=VALUE` and never sourced, and anything already in the environment
wins over it.

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

### Why the install test earns its ten minutes

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
