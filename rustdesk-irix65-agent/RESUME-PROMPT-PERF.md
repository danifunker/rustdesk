# Resume prompt: make the IRIX agent fast enough to use

Continue the IRIX RustDesk agent. Read
`rustdesk-irix65-agent/RESUME.md` first — it is the source of truth for state,
environment, what is verified, what is ruled out, and the mistakes not to
repeat. Do not re-derive anything it records.

**The agent works. It is far too slow.** A peer connects, logs in, and receives
VP8 video continuously; mouse injection works; the emulator bug that used to
wedge the X server is fixed upstream. What is left is that a frame takes tens of
seconds. This session is about that, and only that.

Measured on an emulated 66 MHz R5000 Indy at 1280x1024, with an `xclock`
ticking so the screen genuinely changes:

```
video frames   5 (5 key), 694064 bytes total, over 182 s   = 0.03 fps
```

Divide by roughly 3 for a real Indy, and an O2 should be better again — but
0.03 fps has too far to go for that to rescue it.

## Where the time goes

From `pipeline`, which runs capture → downscale → convert → encode and reports
each stage (emulated; the ratios are what matter):

| stage | 320x256 (1/4) | 640x512 (1/2) |
|---|---|---|
| capture (forced full read) | 376 ms | 365 ms |
| downscale | 548 ms | 301 ms |
| convert to I420 | 87 ms | 308 ms |
| VP8 encode | 464 ms | 1823 ms |
| **total** | **1475 ms** | **2796 ms** |
| idle damage poll | 4 ms | 15 ms |

A full-resolution 1280x1024 encode measured **~6.1 s per frame** on its own.
Encode is 3.9x more expensive for 4x the pixels, so it is linear in area, as
expected.

The idle numbers are the good news: a static desktop costs 2–15 ms per pass to
discover nothing happened. The damage path is not the problem.

## The four things to do, in the order they pay

### 1. Encode at a reduced size. This is the whole game.

The agent captures, converts and encodes the full 1280x1024 every frame.
`Capturer::scaled` exists and works; what is missing is threading a scale factor
through `Video` in `../rustdesk-ppc-agent/src/session.rs` — `I420` and the
encoder are built at `cap.width`/`cap.height`, and `Video::band` converts
native-resolution rows.

Wire it to `image_quality` / `custom_image_quality` while you are there. They
arrive in the message loop and are currently logged and ignored (search
`image_quality`). The peer is *asking* for this and being ignored.

Expect roughly 4x per halving. Getting to 640x512 should be the difference
between unusable and demonstrable.

### 2. Every frame is a keyframe, and that is self-inflicted

All five frames in the run above were keyframes at ~138 KB. The cause is in
`Video::probe`:

```rust
if self.last_key.elapsed() >= KEYFRAME_INTERVAL { self.want_key = true; }
```

with `KEYFRAME_INTERVAL = 10 s`. At 36 s per frame *every* frame is older than
the interval, so every frame is forced to be a key frame — which is several
times more expensive to encode and to send than the inter frame that would have
done, which makes the next frame later still.

It is a feedback loop, not a bug in the rule. Fix (1) and it may resolve itself;
if it does not, the interval wants to be counted in frames rather than seconds,
or skipped when the last frame is still in flight.

### 3. Downscale only what changed

`rd_scale_abgr` walks the whole 1280x1024 canvas regardless of how little moved,
which is why it costs as much as the encode at 1/4. Either scale only the
damaged rectangles, or fuse the scale with the I420 conversion so the canvas is
walked once instead of twice.

### 4. Use the rectangles

`dirty_bands` collapses the server's damage report into 64 band flags and throws
the rest away. `poll_rects` keeps it. Encoding only the changed regions is the
obvious next saving, and it is a much better fit for this server than the band
model inherited from the Mac, where the rectangles had to be guessed.

## Do this before trusting any of it

**The colours have never been verified end to end.** IRIX hands back **A,B,G,R**
in memory; `convert.rs` was written for the Mac's A,R,G,B. `--probe-display`
shows the C shim and the Rust path produce *identical* planes, which proves they
agree with each other and nothing about whether either is right. Decode a
delivered frame and look at it. Red and blue swapped is the failure mode, it is
easy to miss in a still, and optimising a converter that is wrong would be a
waste of a session.

## A caution from the last session

Three "emulator" faults turned out to be mine, and all three were found by
measuring rather than reading:

- a leaked shm segment per failed open — 80 orphans, ~400 MB on a 256 MB guest
- a leaked fd per dead connection — 1309 consecutive `XOpenDisplay failed` from
  one agent while a fresh process captured perfectly
- `display_size()` building an entire capture context — 5 MB shmget,
  `XShmAttach`, `SGICapRegisterInterest`, a ReadDisplay probe — on **every pass
  of the message loop**, because `Capturer::refresh` calls it

That last one was the source of traffic I had been attributing to the emulator
for two sessions. When something looks like a platform problem, measure the
agent first. `ipcs -m`, the `XOpenDisplay` count in `/tmp/agent.log`, and
comparing a long-running agent against a freshly started probe are all cheap and
all found real bugs.

## How to work

Everything is in `rustdesk/rustdesk-irix65-agent` on the `vintage-agents`
branch, beside `rustdesk-ppc-agent`. The two must stay siblings — the portable
modules are included by `#[path]`.

```sh
./ports/rust/build-compat.sh          # once per clone
cd ports/rust/agent-portable && . ../env.sh
cargo +nightly build --release        # 37 s clean, 6 s incremental
```

The build needs no emulator. To run one:

```sh
cd ports/iris-run
DISPLAY=:1 ~/iris-upstream/target/release/iris \
    --config iris.toml --ci --ci-display --cpu r5000
IRIS_SOCKET=/tmp/iris-rdagent.sock iris-ci start      # REQUIRED; see RESUME
```

`~/iris-upstream` is upstream iris at `02c4e155`, which contains the `hostr`
readback fix. **Do not go back to the older build** — it wedges the X server
within one frame. `--cpu` is a runtime option now; the CPU is no longer a build
feature.

On the guest, `/root/agent-restart.sh` refetches the agent, restarts X and
starts it listening. Measure with:

```sh
testpeer 127.0.0.1:21118 hunter2 180   # frames, bytes, fps, keyframe count
pipeline 4 2                           # per-stage timings at 1/4 and 1/2
rustdesk-agent --probe-display         # encoder tuning sweep, converter check
```

Start an `xclock` first, or the screen never changes and the agent is right to
send nothing — which looks exactly like a fault and cost me a round of
investigation.

## Boundaries

- **The disk is nearly full** — under 3 GB free when I stopped. Check before
  building anything large. `~/iris-upstream/target` (1.2 G) and
  `ports/work` (137 M) are the reclaimable things I own.
- **Do not modify `~/repos/iris`.** It has uncommitted work that is not ours,
  and it is not the emulator we run any more.
- **Another session may share this machine.** `pkill -x iris` kills every
  emulator on the box, including theirs — I did that once. Kill the pid whose
  `/proc/<pid>/cmdline` names your config, and note that
  `ps | grep "iris --config"` also matches the bash wrapper.
- **Fold CHD changes** per `RESUME.md` §Disk handling, and keep
  `Indy-IRIX65_dev.chd.bak-before-first-write`. Discarding a diff of pure test
  churn is fine and is what I have been doing; say so when you do.

## When you finish

Update `RESUME.md` in place so the next session starts where you stopped, and
say plainly what got faster, by how much, and what did not. If the numbers are
still bad, an accurate account of where the time goes is worth more than a
hopeful one — the per-stage table above took one `pipeline` run and settled
several arguments.
