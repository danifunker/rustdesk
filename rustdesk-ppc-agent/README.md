# rustdesk-ppc-agent

A RustDesk **agent** — the controlled side — for PowerPC Mac OS X 10.4/10.5.
Blocking I/O, no async runtime, 5 direct dependencies. Built with mrustc; see
[`../docs/powerpc-mrustc-scope.md`](../docs/powerpc-mrustc-scope.md) for why this
exists rather than a port of `src/server/`.

Status: connects, authenticates and streams VP8 to a real client. See
[`docs/BACKLOG.md`](docs/BACKLOG.md) for what is missing — notably change
detection, LAN discovery, and audio.

## Connecting to it

Set a password on the Mac, then start it:

```bash
rustdesk-agent --password <PASSWORD>
rustdesk-agent                       # listens on 0.0.0.0:21118
```

In a RustDesk client, type the **IP address into the ID field** — no port
needed:

```
192.168.99.116
```

The client sees no colon and appends `:21118` (`RELAY_PORT + 1`), which is the
same port the agent defaults to (`RENDEZVOUS_PORT + 2`). `IP:port` also works if
you moved it with `--port`.

Nothing needs enabling client-side: typing an IP triggers a direct connection.
(RustDesk's "Direct IP Access" *setting* is for the machine being controlled —
here that is this agent, which always listens.)

**The G5 will not appear in the client's discovered-machines list** — that uses a
UDP broadcast protocol we do not implement yet (backlog item 2).

### Other commands

```bash
rustdesk-agent --show-id          # the agent's ID
rustdesk-agent --show-key         # public key a peer can pin
rustdesk-agent --probe-display    # framebuffer geometry and per-stage timings
rustdesk-agent --listen 127.0.0.1 --port 5900
```

Configuration lives in `~/.rustdesk-ppc-agent.conf` (mode 0600 — it holds the
signing secret key).

## Building

Host-side checks, which cover everything except the platform layer:

```bash
cargo test
```

For PowerPC, see [`../rustdesk-ppc/README.md`](../rustdesk-ppc/README.md) for the
two-machine model. In short:

```bash
export PPC_HOST=ppctiger
export CC_powerpc_apple_darwin=~/repos/rusty-backup/scripts/ppc-cc-remote.py
export AR_powerpc_apple_darwin=~/repos/rusty-backup/scripts/ppc-ar-remote.py
export PPC_SHIM=~/repos/rusty-backup/rb-cli-ppc/shim/ppc-compat.c
export PPC_LDFLAGS="-L/opt/local/lib -L/Users/admin/ppc-libs/lib -latomic \
    -lMacportsLegacySupport -lgcc_s.1 -lsodium -lvpx"
SODIUM_LIB_DIR=/Users/admin/ppc-libs/lib MRUSTC_TARGET_VER=1.74 \
  ~/repos/mrustc/bin/minicargo . --vendor-dir vendor \
  --target powerpc-apple-darwin \
  -L ~/repos/mrustc/output-1.74.0-powerpc-apple-darwin-g5 \
  --output-dir <out> -j 2
```

Native libraries on the G5 live in `~/ppc-libs` — libsodium and libopus built
from source with gcc10, libvpx and libyuv taken prebuilt from the
leopard-ports/kemonomimi `darwin_9.ppc` mirrors.

## Testing without a RustDesk client

`examples/probe_client.rs` drives the peer side of the protocol and narrates
every step, which makes protocol gaps obvious:

```bash
cargo run --example probe_client -- 192.168.99.116:21118 <password> [<pubkey-b64>]
```

It reports frame count, byte rate, keyframes and time-to-first-frame, and sends a
mouse-move partway through to exercise injection.

## Layout

| path | |
|---|---|
| `src/frame.rs` | length-prefixed framing (mirrors `bytes_codec.rs`) |
| `src/crypto.rs` | secretbox channel, handshake, login hash |
| `src/session.rs` | handshake sequence, message loop, video pump |
| `src/config.rs` | persistent identity and password |
| `src/capture.rs` | `CGDisplayBaseAddress` capture + dirty-band detection |
| `src/convert.rs` | ARGB → I420 |
| `src/encode.rs`, `src/vpx_shim.c` | VP8 via libvpx |
| `src/input.rs` | Quartz Event Services injection |
| `probes/` | C programs establishing the hardware floor |
| `docs/videoperformance.md` | what was measured, and why the design follows |
| `docs/BACKLOG.md` | what is missing |
