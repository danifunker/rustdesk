# rustdesk-ppc-agent

A RustDesk **agent** — the controlled side — for PowerPC Mac OS X 10.4/10.5.
Blocking I/O, no async runtime, 5 direct dependencies. Built with mrustc; see
[`../docs/powerpc-mrustc-scope.md`](../docs/powerpc-mrustc-scope.md) for why this
exists rather than a port of `src/server/`.

Status: connects, authenticates and streams VP8 to a real client, with keyboard,
mouse, trackpad scrolling, the real pointer shape, LAN discovery, screenshots,
clipboard text (see "Where the agent has to run" below — the clipboard is
the one feature that constrains it) and registration with a self-hosted
rendezvous server, so it is reachable by ID rather than only by address.
It reports itself as **1.4.5**, which is a capability declaration rather than a
label — see `REPORTED_VERSION` in `src/session.rs`. There is deliberately **no
audio**; see below for why. [`docs/BACKLOG.md`](docs/BACKLOG.md) has the rest of
what is missing.

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

### By ID, through a rendezvous server

Direct IP only reaches the machine from its own subnet. Point the agent at a
self-hosted RustDesk server and it registers, after which the **ID** works from
anywhere — and that is what discovery advertises too:

```bash
rustdesk-agent --server rustdesk.example.org     # or HOST:PORT; saved, then exit
rustdesk-agent --show-id                         # what to type in the client
rustdesk-agent --key '<base64>'                  # only if hbbr was started with -k
rustdesk-agent --no-server                       # stop registering
```

The client needs the same **Key** as the server (its `id_ed25519.pub`) in
Settings → Network → ID/Relay Server, exactly as for any other peer. Nothing
else changes: the ID goes in the ID field.

`--key` takes that same string and is needed only when the **relay** (hbbr) was
started with `-k`: it goes into `RequestRelay.licence_key`, and a keyed relay
drops a request whose key does not match by simply returning, so the symptom is
a caller waiting on a relay the agent appears never to have joined rather than
any error. An unkeyed hbbr — the common self-hosted case — ignores it. Worth
knowing that hbbs is keyed even with no `-k`, since it auto-generates
`id_ed25519`, while hbbr has no such fallback; the two are configured
separately.

Two things worth knowing about that path:

* **Those sessions are always encrypted**, whatever `--secure` says. A peer
  arriving through a server takes part in the `signed_id`/`public_key` exchange;
  `--secure` exists for the direct-IP listener, where it does not.
* **Remote peers are relayed, local ones are not.** A caller on the same subnet
  is given our address and connects directly; anyone else meets us at the
  relay. The agent does not attempt hole punching — see `docs/BACKLOG.md` item
  12 for why that is a property of the deployment rather than a shortcut.

### Where the agent has to run

Three contexts, and they are not equivalent — this cost real debugging twice, so
it is worth stating plainly:

| started from | window server | clipboard |
|---|---|---|
| ssh login | works | **no** (`PasteboardCreate` → -4960) |
| detached `screen` (what `build-ppc.sh deploy` uses) | works | **no** |
| the LaunchAgent, i.e. the Aqua session | works | yes |
| fully detached (`&`, `nohup`) | **no** — 0x0 display, input only | no |

Note the third row can be reached *from* the first: `launchctl -S Aqua` loads
into the GUI session from an ssh shell. Where the launch is requested and where
the job ends up are different questions.

So **capture is no guide to the clipboard**. For clipboard sync the agent has to
come from `deploy/com.rustdesk.ppc-agent.plist`, and one command does it from
anywhere, ssh included:

```bash
ssh ppctiger '~/rustdesk-ctl'          # deploy/agent-ctl.sh, installed there
ssh ppctiger '~/rustdesk-ctl status'
ssh ppctiger '~/rustdesk-ctl stop'     # KeepAlive means killing it is not enough
```

The flag that makes that possible is **`launchctl -S Aqua`**, which is not in
Leopard's `launchctl load` usage text: plain `launchctl load` filters by the
*caller's* session type, so from ssh it matches an Aqua-only plist against
nothing and says `nothing found to load`. `-S Aqua` names the session to load
*into*. `unload` needs it too.

Only one agent may hold port 21118 — `~/rustdesk-ctl stop` before
`build-ppc.sh deploy`, or the two take turns failing to bind.

Started any other way, everything except the clipboard works and the agent says
so once per session:

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

## Installing it on another Mac

The built binary is **not portable on its own**: `otool -L` names five MacPorts
libraries by absolute path, which exist only on a machine somebody has built a
toolchain on. (libsodium, libvpx, libopus and libyuv are statically linked and
need nothing.) So there are two steps — bundle, then install:

```bash
./build-ppc.sh                       # produces target/ppc/rustdesk-agent
./deploy/bundle.sh                   # produces target/rustdesk-agent-g5.tar.gz
```

`bundle.sh` copies every non-system library the binary needs — walked
*transitively*, because `libgcc_s.1.dylib` is a stub that pulls in two more —
rewrites the load commands to `@executable_path/lib`, checks that nothing
absolute is left, and runs the result before packing it. It does that work on a
PowerPC Mac over ssh, because `install_name_tool` is part of Darwin's cctools
and there is no build of it on the host. About 1 MB of libraries, 4 MB packed.

Then, on the target Mac, untar it and **double-click "RustDesk Agent"**. The app
is the installer and the settings panel both: it shows the ID, whether the
service is running, and the current server, and offers to install or remove the
background service, set the password, ID server, server key and relay, and show
the log. Nobody has to open Terminal.

For a headless install over ssh, the same `install.sh` the app calls is inside
the bundle:

```bash
tar xzf rustdesk-agent-g5.tar.gz
APP="rustdesk-agent-g5/RustDesk Agent.app/Contents/Resources"
sh "$APP/install.sh"                                              # asks for a password
sh "$APP/install.sh" --password hunter2 --server rd.example.org --yes
sh "$APP/install.sh" --uninstall
```

### Why the app is a compiled AppleScript applet

Both alternatives were measured on 10.5.8 rather than assumed, and both fail:

* **A shell script as `CFBundleExecutable`** — the usual trick for this sort of
  thing — is refused by LaunchServices with **-10810** (`kLSUnknownErr`) once it
  is any bigger than trivial. A three-line script app launches; the real one
  never did, and it was not the bundle name, the `Info.plist` keys, the size, or
  anything in `Resources`.
* **Even when such an app does launch, it gets one dialog.** Only the first
  `osascript` is user-interactive; every later one fails with **-1713** ("No
  user interaction allowed"), because a script app never becomes a foreground
  application. Running it from launchd instead fails at the *first* dialog.

`osacompile` produces a real application with Apple's own Mach-O as the
executable, which has neither problem — verified showing three dialogs in a row
including a hidden-answer one. So `deploy/app.applescript` is the UI and
`deploy/agent-helper.sh` is everything it shells out to, which keeps the half
that can be tested from a terminal testable. `bundle.sh` compiles the applet,
checks the executable really is Mach-O, and exercises the helper's `status` and
`menu` commands on every build.

**Websockets and an API server are deliberately absent**, and the app says so
rather than offering dead controls: this agent has no websocket transport and
the open-source server answers websocket registration with `NOT_SUPPORT`
anyway, and the API-server field only matters to a client signing in to an
account, which this agent never does.

Everything lands under `$HOME` — no sudo, and not only out of politeness: the
agent has to run in the user's Aqua session to reach the pasteboard and the
window server, so a system-wide daemon would be the wrong shape even if it were
easier. The installer refuses rather than guesses where it can: it checks the
Mach-O cpusubtype against the machine's actual CPU, and it runs the binary
before writing a LaunchAgent, so a missing library is a message at install time
rather than a job that flaps invisibly under `KeepAlive`.

**It starts at every login**, via `RunAtLoad`, and `KeepAlive` restarts it if it
dies. Login, not boot — a Mac sitting at the login window has no agent, so one
meant to be reachable unattended needs automatic login (System Preferences →
Accounts → Login Options).

Re-running the installer upgrades in place and leaves the password, the server
and the machine's identity alone; `--uninstall` keeps
`~/.rustdesk-ppc-agent.conf` for the same reason, since deleting it would change
the ID and every peer would have to be told the new one. **One installation per
machine**: the LaunchAgent label is fixed, and `launchctl unload` resolves a
plist to its label rather than its path, so a second copy fights the first.

### Running it on a G4

Yes, with a rebuild of the agent — and the dependencies do **not** need one.

```bash
PPC_CPU_FLAGS='-mcpu=7450 -maltivec' ./build-ppc.sh
./deploy/bundle.sh                       # names the tarball -g4 by itself
```

The reason it is only the agent is worth stating precisely, because the obvious
check gives the wrong answer. **The Mach-O `cpusubtype` is not evidence.** It
records what the linker stamped — 100 (`CPU_SUBTYPE_POWERPC_970`) for the agent,
generic `ppc` for every library — and says nothing about the instructions
inside. `deploy/check-cpu-compat.sh` disassembles and counts instead, and what
it finds is:

| | 64-bit ops | `mtocrf` | `lwsync` |
|---|---|---|---|
| the agent, `-mcpu=970` | **362,000** | 1,073 | 334 |
| libvpx, libopus, libsodium, libyuv | none | yes | 15 (vpx) |
| the 7 bundled dylibs | none | yes | 57 |
| Apple's Leopard `libSystem.B.dylib` | none | 498 | none |

The decisive column is the first. `-mcpu=970` implies `-mpowerpc64`, so a G5
build is full of `std`, `ld`, `rldicl`, `mulld` and `fcfid` — 64-bit
instructions that trap on a G4 — while the dependencies contain **not one**.
They were built with no `-mcpu` at all (libsodium's and opus's `config.log`
confirm plain `-g -O2`), so they are already G4 code and only the agent's flags
need changing.

`mtocrf` appears everywhere including Apple's own `libSystem.B.dylib`, which
Leopard shipped to every G4 Mac — so it is settled by the strongest evidence
available: it degrades to `mtcrf` on a pre-2.01 processor, and if it did not, no
G4 would boot. **`lwsync` is the one loose end.** It is in libvpx, libatomic and
libgcc_s, it appears in no Apple library scanned, and gcc here never generates
it from C at any `-mcpu` — so it comes from those libraries' own assembly and a
rebuild would not obviously remove it. It should execute as a full `sync` on a
7450 by the reserved-bit rule, which is correct if slower, but that is the
architecture's promise rather than a measurement. **Nobody has run any of this
on a G4.**

`install.sh` reads the subtype and the machine's `machine` output and refuses a
mismatch with the rebuild command above, rather than letting dyld say "Bad CPU
type in executable". A 7450 build (cpusubtype 10) also runs on a G5, so one
build covers both if you would rather not keep two.

**AltiVec is not a worry on a G4**: every 74xx has it — it is the feature that
defines the G4, present on the 7400, 7410, 7447/7447A, 7448, 7450 and 7455,
across the whole line from the Power Mac to the Mac mini. No G3 has it.

Where the AltiVec actually is, though, is not where this section used to say.
`-maltivec` in `PPC_CPU_FLAGS` emits **nothing** from our own code: gcc does not
auto-vectorise without `-ftree-vectorize`, which is `-O3`, and the shims build
at `-O2`. Measured — the same loop compiles to 0 AltiVec instructions at `-O2
-maltivec` and 10 at `-O3 -ftree-vectorize`. The 42,117 AltiVec instructions in
the agent are **libvpx's**, whose hand-written VP8 kernels account for 53,648 of
them in the archive; everything else (libsodium, libzstd, libyuv, libgcc) has
only a handful of `lvx`/`stvx`/`vxor`, which is vector-register save and restore
in prologues.

So a **G3 would founder on libvpx, not on the shims**. libvpx here is built with
runtime CPU detection (`vp8_machine_specific_config`, and the `_rtcd` tables are
in the archive), so it may well select its generic C paths on a chip without
AltiVec rather than trapping — but that is an inference from symbol names and
nobody has tried it. Treat a G3 as unexplored rather than as ruled out.

One other caveat: the C shims are compiled *on* a PowerPC Mac by the remote-cc
wrapper, so a G4 build still needs a PowerPC machine to build on — the G5 does
fine, being the same toolchain with different flags.

## Why this port has no audio

Not an omission and not a hard part left undone: **the audio path was written,
it worked, and it was taken back out** (`git show 2f4fa3a4d`; reverted by
`b9bd49766`). Measured against a real session, alongside video and clipboard:
48 kHz stereo, Opus at restricted low delay, 10 ms frames, first frame 0.24 s
in, about 2.3 kbit/s.

The reason it is not in the tree is what it captures. CoreAudio on Mac OS X
10.5 can capture an **input device** and nothing else — there is no way for a
process to record what the machine is playing. ScreenCaptureKit, which is how
modern macOS does it, is 12.3+, twelve years after this hardware. Upstream's own
macOS path has the same limitation and the same answer. So the feature as built
sends whatever the default input device hears — line-in on this machine — and
never the sound the G5 is playing, which is the only thing anyone wants from it.
Shipping it would put a working-looking speaker on the client and deliver noise.

**A loopback driver fixes it with no code change.** Such a driver installs
*as an input device* and becomes the default one, and the capture already
written picks it up; Soundflower shipped PowerPC builds for 10.4/10.5. If one is
ever installed here, reverting the revert is the whole job. That is the reason
the work was kept as a commit rather than deleted.

Two things learned on the way that apply to anything else touching CoreAudio on
this machine, and which [`docs/BACKLOG.md`](docs/BACKLOG.md) §3 records in full:
10.5's AUHAL converts channels and sample format but **will not resample**, so
the device's own rate has to be set before the unit is asked for one; and
`AudioComponentFindNext` is 10.6, so a HAL unit has to be found through the
Component Manager instead — which every example written since 2009 gets wrong
for this vintage.

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
| `src/rendezvous.rs` | registers with a server, and answers connection requests |
| `probes/` | C programs establishing the hardware floor |
| `docs/videoperformance.md` | what was measured, and why the design follows |
| `docs/performance-plan.md` | what is left to do about speed, and what each would buy |
| `docs/BACKLOG.md` | what is missing |
