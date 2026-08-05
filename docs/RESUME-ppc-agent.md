# RESUME — RustDesk agent for PowerPC Mac OS X (G5 / Leopard)

Pick-up point. Last updated 2026-08-04, end of the session where it first worked.

**A modern RustDesk client connects to a dual-G5 running Mac OS X 10.5.8 and
displays its screen.** Login works, mouse and keyboard injection work. One
blocker remains: the captured image is frozen after the first frame.

---

## Current state in one screen

```
branch          rustdesk/ppc-agent          (based on upstream 1931cb8c7, v1.1.8)
mrustc          ppc-upstream               2 commits off upstream/master, for PRs
                ppc-async-fixes            5 async fixes, not needed by this agent
the machine     ssh ppctiger  = admin@192.168.99.116
agent binary    ~/rustdesk-agent on the G5, running with -vv, log at ~/agent.log
password        ppctest123
connect         type 192.168.99.116 into the client's ID field (no port)
```

Working: handshake, auth, config/identity, capture (once), ARGB→I420, VP8
encode, VideoFrame delivery, mouse injection, keyboard injection.
30 host tests.

---

## THE BLOCKER: the framebuffer is frozen

`CGDisplayBaseAddress` returns a valid image **once** and then never changes.
`--probe-live` on the G5:

```
t= 0s  dirty bands: 16
t= 1s  dirty bands: 0
...
t= 9s  dirty bands: 0
=> framebuffer appears FROZEN (nothing changed after the first probe)
```

Already ruled out:
- **Not** a stale cached pointer — `CGDisplayBaseAddress` is now re-read every
  frame (in case the window server page-flips). No change.
- **Not** the dirty-band sampling being too coarse — the very first probe sees
  all 16 bands, so the mechanism works; later probes see a genuinely identical
  buffer.

Leading hypothesis: under **Quartz Extreme** the desktop is composited on the
GPU, and this pointer hands back a main-memory buffer that is no longer updated.
If that is right, capture must be replaced — the usual pre-10.6 route is an
OpenGL readback (`CGLCreateContext` with a full-screen pixel format, then
`glReadPixels`). That is real work, not a tweak.

**Next diagnostic:** with a client connected, move the mouse and watch the G5's
*physical* screen. If the cursor moves there while the client's image stays
frozen, input is live and capture is dead — which confirms the hypothesis and
justifies the OpenGL rewrite.

Worth also trying first, cheaply: disabling Quartz Extreme / beam-sync, or
running at a lower colour depth, to see whether the framebuffer becomes live.

---

## The lesson that cost the most time

**Do not let C structs cross the FFI boundary on 32-bit PowerPC.**

`CGPoint` is two doubles passed *by value*. A naive `extern "C"` declaration
delivered x correctly and left y as denormal garbage:

```
asked for     : 640, 360
cursor after  : 640, 0.0000...805
```

`CGEventGetLocation` was broken for the mirror-image reason — a 16-byte *return*
uses a hidden pointer here. The Rust was byte-for-byte equivalent to enigo's
macOS implementation, so reading it found nothing.

The fix, and the rule: build and consume the struct in C, pass only scalars, use
out-params instead of returned structs. Two shims exist for exactly this —
`src/input_shim.c` (CGPoint) and `src/vpx_shim.c` (`vpx_codec_enc_cfg_t`).
Anything new that touches a struct-taking C API should follow suit.

---

## Protocol lessons (all found by testing against a real client)

Three deviations, none of which host tests could have caught:

1. **Direct IP means unencrypted, and the client speaks second.** Upstream's
   `direct_server` calls `create_tcp_connection(.., secure = false)`, and the
   client's direct branch never calls `secure_connection` at all. Sending
   `signed_id` and waiting for `public_key` deadlocks both sides. `--secure`
   re-enables the exchange for peers that know our key.
2. **Login error strings are protocol, not prose.** `handle_login_error`
   compares literals: `"Empty Password"` makes the client prompt,
   `"Wrong Password"` makes it offer a retry. Answering an empty-password probe
   with "Wrong Password" makes it loop silently.
3. **The connection must survive a failed login.** A client needs two attempts
   on one socket — an empty probe, then the real password. Closing after the
   error makes the password dialog flash up and vanish.

Also: `VideoFrame.vp8s` is **field 12**; sending VP8 in `vp9s` (field 6) feeds it
to the client's VP9 decoder. Backported into our proto.

Verified compatible against current `hbb_common`: framing is byte-identical,
`PublicKey`/`Hash` are field-for-field identical, and `LoginRequest`/
`LoginResponse`/`PeerInfo` only gained fields.

---

## Build and deploy

Incremental PowerPC build is **~90 seconds**; clean is ~14 minutes.

```bash
ssh-agent -a /tmp/ssh-agent-ppc.sock >/dev/null 2>&1
SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock ssh-add ~/.ssh/id_rsa    # passphrase, once

export SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock
export PPC_HOST=ppctiger PPC_CPU_FLAGS="-mcpu=970 -maltivec" PPC_JOBS=2
export PPC_SHIM=~/repos/rusty-backup/rb-cli-ppc/shim/ppc-compat.c
export PPC_LDFLAGS="-L/opt/local/lib -L/Users/admin/ppc-libs/lib -latomic \
    -lMacportsLegacySupport -lgcc_s.1 -lsodium -lvpx"
cd ~/repos/mrustc
SODIUM_LIB_DIR=/Users/admin/ppc-libs/lib \
CC_powerpc_apple_darwin=~/repos/rusty-backup/scripts/ppc-cc-remote.py \
AR_powerpc_apple_darwin=~/repos/rusty-backup/scripts/ppc-ar-remote.py \
MRUSTC_TARGET_VER=1.74 ./bin/minicargo ~/repos/rustdesk/rustdesk-ppc-agent \
  --vendor-dir ~/repos/rustdesk/rustdesk-ppc-agent/vendor \
  --target powerpc-apple-darwin \
  -L output-1.74.0-powerpc-apple-darwin-g5 --output-dir <out> -j 2
```

Deploy and restart (kill by `comm`, never by matching the full command line —
a pattern that matches your own shell will kill it):

```bash
ssh ppctiger 'ps -axo pid,comm | awk "\$2 ~ /rustdesk-agent/ {print \$1}" \
  | while read p; do kill -9 $p; done; sleep 2'
scp <out>/rustdesk-agent ppctiger:~/rustdesk-agent
ssh ppctiger 'rm -f ~/agent.log; nohup ~/rustdesk-agent --port 21118 -vv > ~/agent.log 2>&1 &'
```

Diagnostics:

```bash
ssh ppctiger '~/rustdesk-agent --probe-display'   # per-stage timings
ssh ppctiger '~/rustdesk-agent --probe-live'      # capture liveness + injection self-test
ssh ppctiger 'tail -f ~/agent.log'                # protocol trace
cd rustdesk-ppc-agent && cargo test               # 30 host tests
cargo run --example probe_client -- 192.168.99.116:21118 ppctest123
```

Note the agent serves **one peer at a time**, so probe_client fails while a real
client is connected.

---

## Performance (measured, 1920x1080)

| stage | cost |
|---|---|
| dirty-band probe | 6 ms |
| capture (VRAM → RAM) | 347 ms |
| ARGB → I420 | 180 ms |
| VP8 encode | 83 ms |
| **full-screen change** | **610 ms** |
| **idle** | **6 ms** |

Capture is a hardware floor at ~23 MB/s, so **resolution is the lever** —
1024x768 is 126 ms. Encode is the *cheapest* stage; tuning the codec is the
least valuable thing available. Full detail and the reasoning in
[`../rustdesk-ppc-agent/docs/videoperformance.md`](../rustdesk-ppc-agent/docs/videoperformance.md).

---

## What is not implemented

See [`../rustdesk-ppc-agent/docs/BACKLOG.md`](../rustdesk-ppc-agent/docs/BACKLOG.md).
Headlines: LAN discovery (the G5 will not appear in the client's discovered list;
UDP 21119, `PeerDiscovery` needs backporting), audio (libopus is already on the
machine), clipboard, cursor shape, multi-monitor, running as a service.

Sessions are **unencrypted** in direct-IP mode. That is upstream's behaviour, and
the user has accepted it for now.

---

## Native libraries on the G5

In `~/ppc-libs`: libsodium 1.0.18 and libopus 1.3.1 built from source with
gcc10; libvpx 1.16.0 **+altivec** and libyuv taken prebuilt from the
leopard-ports / kemonomimi `darwin_9.ppc` mirrors. There are no 32-bit Leopard
binaries on macos-powerpc.org — its `packages/` is Snow Leopard and its
`packages_ppc64/` is the wrong architecture.

MacPorts cannot supply these: every port wants `gcc16` as a build dependency,
which on Leopard would build from source.
