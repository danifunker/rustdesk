# rustdesk-ppc-agent

A RustDesk **agent** — the controlled side — for PowerPC Mac OS X 10.4/10.5.
Blocking I/O, no async runtime, 5 direct dependencies. Built with mrustc; see
[`../docs/powerpc-mrustc-scope.md`](../docs/powerpc-mrustc-scope.md) for why this
exists rather than a port of `src/server/`.

Status: connects, authenticates and streams VP8 to a real client, with keyboard,
mouse, trackpad scrolling, the real pointer shape, LAN discovery, screenshots
and clipboard text (see "Where the agent has to run" below — the clipboard is
the one feature that constrains it).
It reports itself as **1.4.5**, which is a capability declaration rather than a
label — see `REPORTED_VERSION` in `src/session.rs`. See
[`docs/BACKLOG.md`](docs/BACKLOG.md) for what is missing — notably audio,
clipboard and multi-monitor.

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

The G5 also answers the UDP broadcast on 21119 that populates a client's
local-network list, so it can be picked from there instead of typed (`src/lan.rs`).

### Where the agent has to run

Three contexts, and they are not equivalent — this cost real debugging twice, so
it is worth stating plainly:

| started from | window server | clipboard |
|---|---|---|
| ssh login | works | **no** (`PasteboardCreate` → -4960) |
| detached `screen` (what `build-ppc.sh deploy` uses) | works | **no** |
| the LaunchAgent, i.e. the Aqua session | works | yes |
| fully detached (`&`, `nohup`) | **no** — 0x0 display, input only | no |

So **capture is no guide to the clipboard**. If you want clipboard sync, the
agent has to come from `deploy/com.rustdesk.ppc-agent.plist`, and that has to be
loaded from Terminal.app *on the G5* — an ssh session reaches a different
launchd. Only one agent may hold port 21118; see the plist's own header.

Without it everything else works and the agent says so once per session:

```text
INFO clipboard unavailable: the pasteboard needs the Aqua session, ...
```

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

### Type-checking the platform layer

**mrustc does not borrow-check.** The PowerPC build will happily compile code
real rustc rejects, so the macOS-gated modules need checking against a compiler
that does. There is no macOS host here, so the cfg is forced instead:

```bash
rsync -a src examples Cargo.toml Cargo.lock build.rs .cargo vendor /tmp/mac-check/
cd /tmp/mac-check
sed -i 's/kind = "framework"/kind = "dylib"/' src/capture.rs src/input.rs
printf 'fn main() {}\n' > build.rs        # see below -- this line is the point
RUSTFLAGS='--cfg target_os="macos" -A explicit_builtin_cfgs_in_flags -A unexpected_cfgs' \
  cargo check --all-targets
```

**Do not skip the `build.rs` line.** The real `build.rs` reads
`CARGO_CFG_TARGET_OS`, which is the *actual* target and so is `linux` however the
RUSTFLAGS are set — so it sets `no_vpx`, and everything behind
`#[cfg(all(target_os = "macos", not(no_vpx)))]` is quietly excluded. That is
`src/encode.rs`, `Video`, and the whole video half of the message loop: the check
passes in seconds and has looked at none of it. The tell is a run full of
`constant KEYFRAME_INTERVAL is never used` warnings.

Emptying `build.rs` leaves `no_vpx` unset, which is what gets the video path
compiled; `cargo check` does not link, so the `extern` blocks only need to
resolve as declarations. Worth confirming the gating actually flipped by breaking
something inside the region on purpose and watching the error appear.

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

It reports frame count, byte rate, keyframes and time-to-first-frame, and sends
four things partway through to exercise the agent: a mouse-move, a
`refresh_video(false)` that must produce nothing, the refresh button a 1.2.4+
client sends, and a screenshot request whose PNG is written to `$PROBE_SHOT`
(default `/tmp/probe-shot.png`) so a real decoder can be pointed at it.

`examples/png_check.rs` writes PNGs beside the RGB they must decode to, for
checking the encoder against a library that is not ours:

```bash
cargo run --example png_check -- /tmp/out
python3 -c "from PIL import Image; im=Image.open('/tmp/out/display.png'); \
  print(im.mode, im.size, im.convert('RGB').tobytes()==open('/tmp/out/display.raw','rb').read())"
```

## Layout

| path | |
|---|---|
| `src/frame.rs` | length-prefixed framing (mirrors `bytes_codec.rs`) |
| `src/crypto.rs` | secretbox channel, handshake, login hash |
| `src/session.rs` | handshake sequence, message loop, video pump |
| `src/config.rs` | persistent identity and password |
| `src/capture.rs` | `CGDisplayBaseAddress` capture + dirty-band detection |
| `src/convert.rs`, `src/convert_shim.c` | ARGB → I420, and ARGB → PNG scanlines |
| `src/encode.rs`, `src/vpx_shim.c` | VP8 via libvpx |
| `src/png.rs` | PNG for `ScreenshotResponse`, via the system zlib |
| `src/clipboard.rs`, `src/clipboard_shim.c` | clipboard text, via libzstd and the Pasteboard Manager |
| `src/input.rs` | Quartz Event Services injection |
| `src/lan.rs` | answers the UDP discovery broadcast |
| `probes/` | C programs establishing the hardware floor |
| `docs/videoperformance.md` | what was measured, and why the design follows |
| `docs/BACKLOG.md` | what is missing |
