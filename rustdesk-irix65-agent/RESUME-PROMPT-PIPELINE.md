# Resume prompt: the IRIX agent has a package and a pipeline

Continue the IRIX RustDesk agent. Read `rustdesk-irix65-agent/RESUME.md` first —
it is the source of truth for state, environment, what is verified, what is
ruled out, and the mistakes not to repeat. Do not re-derive anything it records.

**The agent works, its settings panel works, and there is a `.tardist` that
installs on a stock IRIX 6.5 machine.** The pipeline that builds it boots its
own emulated guest and disposes of it, so a release needs nobody sitting there.
What is left is small, plus two things that moved underneath all of it while it
sat.

## Where it stands

Six commits, most recently *"irix65-agent: the install test is not part of a
release"*. One command builds a release:

```sh
scripts/release.sh --boot          # cold: boots a guest, disposes of it, ~10 min
scripts/release.sh --no-inst       # no guest at all: binaries + tarball
```

and the last one produced `dist/rustdesk-agent-20260824-36e916bf3-n32.tardist`,
which was installed in a guest and run:

```
I  rustdesk_agent  2026082441  RustDesk agent for IRIX
/usr/sbin/rustdesk-agent --show-id  ->  ff6izj02b     # LD_LIBRARYN32_PATH unset
```

`docs/PACKAGING.md` is the whole account. `scripts/README.md` is the short one.

## READ THIS BEFORE YOU BUILD ANYTHING

Two things changed after the work was finished, and both are quiet:

**1. The branch was rebased, so the shipped artifacts name a commit that no
longer exists.** `dist/` is stamped `36e916bf3`; that object is not in the
repository. The same commits are there under new hashes (`2f5ad77a5` and
back), and `git log --grep='irix65-agent:'` finds them. Nothing is lost — but
do not try to reproduce that build by checking out its stamp, and expect the
hashes in this file to have moved again by the time you read it. Search by
subject, not by hash.

**2. The shared PowerPC tree has moved, and it touched IRIX.** The agent
includes `../rustdesk-ppc-agent/src/*` by `#[path]`, and the SPARC/Solaris
effort has since changed `input.rs`, `main.rs`, `rendezvous.rs`, `session.rs`
and `sys.rs` — about 235 lines, including new `#[cfg(any(target_os = "irix",
target_os = "solaris"))]` blocks. One commit says so outright: *"Solaris 10 has
no SO_RCVTIMEO either, so it takes IRIX's poll path."*

So **the next `scripts/build.sh` produces a different agent from the one in
`dist/`, and nothing in it has been compiled or run on IRIX.** That is the first
thing to do, below. The IRIX tree itself is untouched — `git diff <mine>..HEAD
-- rustdesk-irix65-agent` is empty.

## What to do first

1. **Rebuild and re-run the install test**, because of (2) above:

   ```sh
   scripts/release.sh --boot
   scripts/iris-install-test.sh --boot --tarball
   ```

   The install test is deliberately **not** part of a release — it doubles the
   run and almost all of that is `inst` grinding on an emulated R5000 — but a
   change to shared code that edits IRIX `cfg` blocks is exactly the occasion
   for it. Expect `ff6izj02b`, `agent=/usr/sbin/rustdesk-agent`, and no `rld:`
   anywhere.

2. **Then run the panel against it**, which is the only thing that exercises
   the agent's lifecycle end to end:

   ```sh
   # on the guest, after installing
   RD_GUI=/usr/sbin/rustdesk-agent-gui RD_HELPER= sh /tmp/gui-press.sh
   ```

   It clicks Apply, Start and Stop with injected input and checks the config
   and the process list. `docs/panel.png` is what a good run looks like.

## The traps, and they are all one trap

Three of the faults this pipeline hit were the same mistake in different
clothes, and it is worth holding the general form in your head because it will
show up again:

> **Ask whether the thing works, not whether a proxy for it looks right.**

- A wedged X server accepts connections and never answers, so it looks exactly
  like a broken client. Prove the server answers *someone* — `/root/tmo 25
  xdpyinfo` — before blaming your own binary.
- `iris-ci boot` waits for a console login banner that this image prints **five
  minutes after the guest is usable**, and via a PROM menu it autoboots past
  anyway. `iris-guest.sh` polls until a command comes back instead.
- `iris-ci login` **fails when the console is already logged in** — it types
  `root` at a shell and reports it never saw what it wanted. `guest_login` asks
  whether a command runs first.

Four more, each of which cost a run:

- **`iris-ci get` guesses the console's shell wrong**, sending csh syntax to
  bash, and fails claiming it "needs a shell on the serial console" while the
  shell is right there. `guest_get` retries; one retry has always been enough.
  It has missed twice, **both times on the second `get` of a run**. Worth
  reporting upstream — `get` has no `--shell` flag the way `run` does.
- **A full build host panics the guest.** The overlay write fails and IRIX
  treats that as fatal, ten minutes in, naming nothing. There is a `df` floor
  now (`IRIS_GUEST_MIN_MB`), and the machine has form: it filled to 100% from
  something outside the session once already.
- **Editing a shell script while it is executing corrupts the run.** `sh` reads
  incrementally. This bit `iris-guest.sh` mid-cleanup.
- **`eval "$(cmd)"` hides failure from `set -e`**, because eval returns the
  status of the string it evaluated and a dead command evaluates to nothing.

And the one from the GUI work that will outlive all of these: **IRIX `ps -e`
truncates the command to eight characters**, so `grep rustdesk-agent` against it
matches nothing — and `ps -e -o pid,args | grep rustdesk-agent` matches too
much, including the helper's own shell once the helper lives in
`/usr/lib/rustdesk-agent/`. Match the basename of `argv[0]` exactly.

## What is open

- **Dani's deployment.** The CLI, the panel and the helper all do it and it is
  verified on IRIX. It needs three values only Dani has: the **hbbs hostname**,
  the **server key**, and the **console URL** with its CA if it is private.
  Then it is three Applies in the panel.
- **The package has never been through an upgrade** between two *different*
  versions, and `versions remove rustdesk_agent` is untested.
  `iris-install-test.sh --remove` runs the removal half.
- **Most of the panel's buttons are unpressed** — Restart, Refresh, the menus,
  and the five Apply buttons other than Relay. Same callback, different client
  data, so the risk is low; the distinction between "proven" and "the same code
  with a different argument" is why this line exists.
- **No o32 flavor.** `--abi` and `stage_inst_inputs` take one; there is no o32
  build of the agent and no 5.3 image. `../irixscsitb` runs exactly this as a
  two-flavor matrix and is the thing to copy.
- **No login session, and the agent has never survived its X server dying.**
  Both need real hardware or a fixed emulator; §A REAL LOGIN SESSION in
  `RESUME.md` has the detail and the answer to the authority question, which is
  good.
- **`inst` is where a run's time goes** — ten minutes of twenty when the
  install test runs at all. Snapshots (`iris-ci save`/`restore`, ~145 ms) would
  help and want an invalidation rule first.

## Boundaries that still hold

- **Do not modify `~/repos/iris`.** Note what you wish were different instead.
  `docs/ISSUE-nat-inbound-stall.md`, `docs/ISSUE-rex3-wedge.md` and
  `docs/ISSUE-xdm-wedge.md` are written up and ready to send.
- **Do not start IRIX 5.3.** Deferred by decision.
- **The disk image is never written to.** The pipeline runs the guest on a
  copy-on-write overlay and throws it away; `~/Indy-IRIX65_dev.chd` has been
  `md5 0604bd26d8f8c0801e5a8778c3834b78` through every run, including the ones
  that failed. Keep it that way, and keep
  `Indy-IRIX65_dev.chd.bak-before-first-write`.
- **The machine is shared.** Another effort runs its own emulator on the
  default `/tmp/iris.sock`; iris *deletes and rebinds* whatever socket it is
  given. The pipeline's guest picks a per-run socket and takes no port
  forwards, so it cannot steal anything — do not undo that.

## When you finish

Update `RESUME.md` in place, and say plainly what works and what does not. If
something is still broken, an accurate description of the failure is worth more
than a hopeful one.
