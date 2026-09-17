Continue the PowerPC Mac OS X RustDesk agent. The documentation in this
directory is the source of truth and none of it needs re-deriving:

* `README.md` — what the agent does, every flag, and how a client reaches it.
* `docs/BUILD.md` — the toolchain, the cross-build, and the exact mbedTLS
  recipe including the two flags that are not optional.
* `docs/BACKLOG.md` — §12 rendezvous (and the 2026-08-18 re-check of websockets
  against CortenDesk's fork), §14 the console/API client and TLS, §15 the
  double application menu.

**1.0.0 is built, deployed and working.** It serves sessions, registers by ID,
and reports in to a CortenDesk console over HTTPS. Measured on the G5, not
inferred: `TLSv1.3, TLS1-3-AES-256-GCM-SHA384`, inventory accepted, and the
console showing `PowerPC G5 @ 2.3 GHz (2 cores)` / `4 GB` / `Mac OS X 10.5`.
That last line is also the proof `sys`'s sysctl reader is right on big-endian —
a fixed-width read of `hw.memsize` would have produced nonsense.

## The machine

`ppctiger` = `192.168.99.116`, user `admin`, ssh keys work. **It dual-boots and
the two halves are not interchangeable:**

| boot | what it is for |
|---|---|
| **10.5.8 Leopard** | the build environment. `gcc10-bootstrap`, `~/mrustc`, `~/ppc-libs` with libsodium/libvpx/libyuv/libopus **and mbedTLS**, which was hand-built there — see `docs/BUILD.md` |
| **10.6.8** | MacPorts 2.12.5 with 161 ports, `mrustc` as a *port* (targeting Rust 1.90, not 1.74), `mbedtls3` and `curl-ca-bundle` available |

**There is no passwordless sudo on either.** Ask for anything needing root and
say exactly which command. `port install` is the usual one.

`/opt/local/bin` is **not on the non-login PATH**. Export it before running
`port`, or every query fails silently and you read the fallback as a finding —
which is exactly what happened once, in the same family as §1d's `fb-vigil` and
§12's `docker-proxy`.

Builds run from this Linux box and compile the C on the Mac over ssh, so
`SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock` must hold the key.

## Priorities, in order

### 1. A fresh release from the committed tree

Everything currently shipped was built with `--allow-dirty` and is stamped
`91f7c3750+dirty` in `MANIFEST.txt`, Get Info and the About window — not
reproducible from a commit. The tree is clean now, so a rebuild produces an
honest stamp. That is the whole point of doing it.

```bash
cd /home/dani/repos/rustdesk/rustdesk-ppc-agent
export SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock
# The Mac's mbedTLS for the PowerPC target; a local one for the host tests.
export MBEDTLS_INCLUDE_DIR_powerpc_apple_darwin=/Users/admin/ppc-libs/include
export MBEDTLS_LIB_DIR_powerpc_apple_darwin=/Users/admin/ppc-libs/lib
export MBEDTLS_INCLUDE_DIR=/path/to/host/mbedtls/include
export MBEDTLS_LIB_DIR=/path/to/host/mbedtls/library
./build-release.sh --host ppctiger
```

**The target-suffixed spelling is not optional.** `build-release.sh` runs the
host test suite and the cross build in one process tree; set only the plain
names and the host build inherits a `-I` that exists only on the G5, and cc-rs
dies on it. That cost one release run. Without a host mbedTLS the plain pair can
be left unset — the host build then compiles with `no_tls` and still passes all
188 tests, it just does not cover the TLS path.

Before packaging, **stage a CA bundle at `/tmp/cacert.pem` on the Mac** if the
Leopard boot still lacks `curl-ca-bundle`, or `bundle.sh` warns and ships an
app that cannot verify any https console. Download it here, not there — that
Mac's own TLS cannot reach a modern download, which is this whole feature in
miniature.

Expect 30–40 minutes: host tests, G4, G5, universal fuse, `.app`, DMG.

### 2. Update the ports repo

`/home/dani/repos/powerpc-ports`, remote `git@github.com:danifunker/powerpc-ports.git`,
already on branch `rustdesk-ppc-agent`, 2617 ports. It already carries
`net/rustdesk-ppc-agent/Portfile` and `lang/mrustc/` with its patches.

Its copy of the Portfile is **stale at 0.1.0**. The canonical one is
`rustdesk-ppc-agent/macports/net/rustdesk-ppc-agent/Portfile`; the diff is the
version, the distname, and the two new dependencies (`mbedtls3`,
`curl-ca-bundle`) plus `MBEDTLS_DIR` in `build.env`. No new Rust crates were
added — the JSON and HTTP layers are hand-rolled precisely so `cargo.crates`
did not have to grow — so that list should not need touching. Verify rather
than assume.

**The checksums cannot be right until the tag exists.** They are still the
`ppc-agent-0.1.0` tarball's, and the Portfile says so in a comment. Order:

1. Push the work and tag `ppc-agent-1.0.0` on `danifunker/rustdesk`.
2. On the Mac (10.6 boot, `/opt/local/bin` on PATH):
   `sudo port -v checksum rustdesk-ppc-agent` — needs root, so ask.
3. Copy the new checksums into both copies of the Portfile and drop the STALE
   comment.
4. Prove it: `sudo port install rustdesk-ppc-agent` on a machine with
   `mbedtls3` present. Nobody has ever run the port build end to end — it is
   the one path in this project that is written and unproven.

Keep the two copies in step. The one in the rustdesk tree is what gets edited;
the ports repo is where it is published.

## What is proven, and what is not

Proven on hardware: the TLS layer and its three rejection paths, the console
first-contact sequence, the inventory values, the universal binary on a G5, and
the settings app compiling under Cocoa.

Not proven, and worth saying plainly rather than assuming:

* **The web client reaching the G5.** BACKLOG §12 shows `relay_server.rs` pairs
  by uuid across transports, so a browser on `wss://` and this agent on native
  TCP should meet at the relay with no agent change at all. Nobody has tried
  it. It is a test, not a feature — and if it works, agent-side websockets buy
  nothing.
* **Agent-side websockets are still blocked**, and not by TLS. CortenDesk's
  fork answers `RegisterPk` with `NOT_SUPPORT` in `handle_tcp`, which is what
  its websocket loop calls. Verified in the server actually deployed. Do not
  reopen this without new evidence from the server side.
* **The G4 slice has never run on a G4.** It is verified by instruction scan
  only; the MANIFEST says so.
* **The port build** (above).
* **The menu fix has not been seen.** §15 measures it — one menu instead of
  two — but `screencapture` over ssh returns black when the display is asleep,
  so no one has looked at the menu bar. Ask the user rather than claiming it.

## Boundaries

* **`rustdesk-ppc-agent/src/` is two ports' code now.** The SPARC agent builds
  `../rustdesk-ppc-agent/src/main.rs` through its `Cargo.toml`, and its
  `build.rs` compiles this tree's `convert_shim.c` and `tls_shim.c`. It even
  keeps the library name `rustdesk_ppc_agent` so the `#[path]` references
  resolve. Adding a `target_os` arm is expected; changing what the PowerPC path
  *does* is not, and neither is restructuring a shared file quietly. Check the
  negated `cfg` forms too — `cfg(any(…))` and `cfg(not(any(…)))` are different
  strings.
* **Do not weaken certificate verification.** There is deliberately no flag to
  skip it, and the CA bundle is explicit because this platform's trust store is
  a decade stale.
* Do not commit with `--allow-dirty` stamps as the final artifact. It is fine
  mid-session and wrong to hand to anyone.

## How to work

* **Measure on the machine.** Every number in the backlog came from a probe.
  When you catch yourself reasoning about what Leopard probably does, write the
  probe instead — and give the instrument its own control, because the two
  worst hours in this project's history were both a broken instrument reading
  as a result (§1d, §12, §13, and the `port` PATH above).
* A feature can be "on" and invisible. The CA bundle bug shipped, registered
  fine, and simply never appeared in a device list — the one line saying why
  was at ERROR in a log nobody opens while everything looks healthy. When
  something is configured and absent, read the log before theorising.
* When a build succeeds but behaves like older code, suspect the build-script
  cache: minicargo does not honour `cargo:rerun-if-changed`, which is why
  `build-ppc.sh` deletes the marker when a shim is newer.
* Deploying: `install.sh --yes` is the supported upgrade in place and keeps the
  config, ID and password. The LaunchAgent runs
  `~/rustdesk-ppc-agent/rustdesk-agent`, **not** the copy inside the `.app`, so
  replacing only the app gives a new UI driving an old agent.

When you finish or run out of road, update this file in place and say plainly
what worked, what did not, and what you are unsure about. An accurate
description of a failure is worth more than a hopeful one.
