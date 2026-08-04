# RESUME — RustDesk agent for PowerPC Mac OS X (G5 / Leopard)

Pick-up point for the PowerPC port. Written 2026-08-04.

**Goal:** run a RustDesk *agent* on a dual-G5 running Mac OS X 10.5.8, so a
modern client can connect **into** it (the G5 is the controlled machine). Video
+ input are phase 1, audio is wanted in phase 1, clipboard is phase 2.

Read [`powerpc-mrustc-scope.md`](powerpc-mrustc-scope.md) for the *why*; this
file is the *where we are and what's next*.

---

## The one decision that shapes everything

**We are NOT porting `src/server/`. We are building a small standalone agent.**

RustDesk is tokio top to bottom, and mrustc has two async gaps that are deep and
unbounded — an `async {}` block nested inside an `async fn` (which is exactly
`tokio::spawn(async move { .. })`, used unavoidably in
`rendezvous_mediator.rs:59,75`) and `Pin<&mut Self>` vtable thunks. Neither is
optional for a real port.

A single-peer agent needs no async at all: blocking I/O and a couple of threads.
That removes both gaps and takes the dependency graph from **376 crates**
(current RustDesk) → **175** (ported 1.1.8) → **18** (this agent).

The `ppc-agent` branch still carries the *port* attempt (manifest deviations,
headless CM, build tooling). It is not wasted — it is where the protocol was
mapped, and the headless-CM/self-hosted work applies either way — but the
active line of work is `rustdesk-ppc-agent/`.

---

## Where things stand

### Done and verified

- **mrustc**: 5 async fixes, committed as one commit on branch
  `ppc-async-fixes` in `~/repos/mrustc` (`16cbf311`). Regression set: 6 pass,
  2 known-remaining (the two gaps above). See the commit message for each fix.
- **`rustdesk-ppc-agent/`**: crate scaffolded, **13 tests passing**.
  - `src/frame.rs` — RustDesk's length-prefix framing on blocking `std::io`,
    checked against upstream's own boundary vectors.
  - `src/crypto.rs` — secure channel + handshake + login hash, with the wire
    spec written down in the module header. Tests pin the details that would
    silently break: nonce is little-endian **on a big-endian host**, counters
    are **pre-incremented** (first frame uses nonce 1, not 0), the box nonce in
    the key exchange is **all zeros**, `sign::sign` is the **combined** form.
  - protobuf generated from `../libs/hbb_common/protos/*.proto`, so the wire
    format is shared with the peer rather than re-specified.
- **G5 is reachable again**, see "SSH" below.

### Not started

- `src/session.rs` — handshake sequence + message loop (lift from
  `src/server/connection.rs`).
- `src/capture.rs` — **the one genuinely new piece.** `CGDisplayStream` is
  10.8+; Leopard needs `CGDisplayCapture` + `CGDisplayBaseAddress` /
  `CGDisplayBytesPerRow`.
- `src/encode.rs` (libvpx VP8), `src/input.rs` (`CGEventPost`),
  `src/convert.rs` (BGRA→I420, hand-written to avoid libyuv).
- Native libraries on the G5 — see below. **Nothing has been built or installed
  on the G5 yet; all G5 access so far has been read-only probing.**

---

## The G5

`ssh ppctiger` → `admin@192.168.99.116`. Mac OS X **10.5.8** (Darwin 9.8.0,
RELEASE_PPC), **dual PowerPC 970 @ 2.3 GHz, 4 GB RAM**, 27 GB free.

### SSH — the key is passphrase-protected

Claude's shell env does not persist between commands, so an agent at a *fixed
socket path* is required:

```bash
ssh-agent -a /tmp/ssh-agent-ppc.sock >/dev/null 2>&1
SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock ssh-add ~/.ssh/id_rsa   # prompts once
```

Then prefix every remote call with `SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock`.
`~/.ssh/config` already has the `ppctiger` entry with
`PubkeyAcceptedAlgorithms +ssh-rsa` (Leopard's sshd needs it).

### Already installed — nothing to do

gcc **10.5.0** (`/opt/local/libexec/gcc10-bootstrap/bin/gcc`, needed for C11
`<stdatomic.h>`), **MacportsLegacySupport**, SDKs 10.3.9 / 10.4u / 10.5,
cctools + ld64 (`darwin-xtools`), gmake, MacPorts 2.12.5.

### Missing — the actual gap

| library | for | notes |
|---|---|---|
| **libsodium** | all protocol crypto | `libsodium-sys` takes `SODIUM_LIB_DIR`, so point it at the PPC build |
| **libvpx** | VP8 encode | the slow one to build |
| **libopus** | audio (phase 1) | via `magnum-opus`, which ships a checked-in `src/opus_ffi.rs` — its bindgen call can be script-overridden away |
| ~~libyuv~~ | BGRA→I420 | **skip** — ~30 lines to hand-write; its port drags cmake + ninja + libjpeg-turbo |

### Two ways to get them, and why the obvious one is a trap

**MacPorts** lists **`gcc16` as a build dependency** for all three — a full GCC
bootstrap on a 2.3 GHz G5. It also needs `git` (absent), and the G5's curl is
**7.16.4 / OpenSSL 0.9.7l**, which cannot do HTTPS at all (plain HTTP works), so
MacPorts cannot fetch its own sources. *(There is reportedly a newer curl at
`/opt/bootstrap/bin/curl` — unverified.)*

Note this limitation does **not** affect our build: mrustc, vendoring and
minicargo all run on the Linux host; the G5 only compiles the emitted C.

**Recommended:** fetch the three tarballs on the Linux host, rsync them over,
and `configure && make` with the **existing gcc10** into a private prefix
(`~/ppc-libs`) — hours instead of days, one toolchain instead of two, and
`rm -rf`-reversible. Mixing gcc16-built dylibs with our gcc10-compiled objects
is a libgcc-runtime risk worth avoiding.

gcc16 is still worth having as a *fallback* if a library won't build cleanly on
Leopard (MacPorts carries real patches for this platform). It blocks nothing, so
it can install in the background.

---

## Next steps, in order

1. **Build libsodium** for PPC into `~/ppc-libs` (quick, unblocks all crypto).
2. **`src/session.rs`** — handshake + message loop. Can be written and tested on
   the Linux host against a real RustDesk client, before any PPC work.
3. **Build libvpx + libopus.**
4. **`src/capture.rs`** — the Leopard capture path. The one piece with no
   upstream to copy.
5. **First end-to-end transpile** through mrustc for `powerpc-apple-darwin`,
   then compile on the G5 via `ppc-cc-remote.py`.

Steps 2 and 4 are independent of the native libraries and of each other.

---

## Commands

```bash
# agent crate (host tests — this is the fast inner loop)
cd rustdesk-ppc-agent && cargo test

# the PORT attempt (separate line of work, see rustdesk-ppc/README.md)
PPC_STUB_CC=1 rustdesk-ppc/build-ppc.sh hbb      # front-end only, no G5 needed
rustdesk-ppc/build-ppc.sh vendor                 # re-resolve + re-vendor

# mrustc
cd ~/repos/mrustc && git checkout ppc-async-fixes && make -j$(nproc)
```

## Gotchas already paid for

- **minicargo cannot read `git =` dependencies.** `rustdesk-ppc/patches/git-deps.py`
  rewrites them to path deps into the vendor dir, and reverses (cargo needs the
  real URLs to re-resolve). The build driver applies it automatically.
- **`libc` must be pinned `=0.2.174`.** 0.2.175 introduced the `src/new/` module
  tree, whose cfg_if'd glob re-exports mrustc cannot resolve on Apple targets.
- **`bytes` must be pinned `=1.10.1`.** 1.11+ trips a mrustc lifetime bug.
- **`protobuf-codegen-pure` aborts under modern std**, so every crate that runs
  it needs `[profile.*.build-override] debug-assertions = false`.
- **`psutil` looks de-forkable and is not** — its source is byte-identical to
  published 3.2.1, but the fork exists to bump `platforms` off 0.2.1, and every
  `platforms` 0.2.x is yanked. See the audit table in the scope doc §3f.
- **`cpal` *was* de-forkable** — the fork only touches ALSA and Windows ASIO;
  CoreAudio is byte-identical.
