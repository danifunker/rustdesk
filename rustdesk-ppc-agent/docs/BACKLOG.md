# Backlog

Ordered roughly by what unblocks the most. Anything already measured links to
[`videoperformance.md`](videoperformance.md) rather than repeating numbers.

---

## ~~1. Change detection doesn't fire — capture may be stale~~ (fixed)

Neither of the causes guessed here was right, and the more expensive guess was
the more wrong one.

`CGDisplayBaseAddress` is perfectly live: measured over 140 s with nothing
forced, six distinct screens, every in-process read agreeing with a freshly
exec'd process. No OpenGL readback was needed.

The sampling was not too sparse either. It was reading one byte per sampled
pixel at a stride that is a multiple of the 4-byte pixel, so every byte it read
was the alpha channel — 0xff across the whole desktop. The checksum was a
constant, so no screen ever differed from any other. The single keyframe each
peer received came from the `invalidate()` at connect.

Sampling R, G and B fixed it. `hash_row` in `src/capture.rs` carries the
warning, and two host tests fail if anyone reintroduces the stride.

A capture/release cycle was added in between, on the theory that the mapping
needed republishing — it appeared to work because changes were being forced with
`killall Dock` and launchd throttles respawns to ten seconds. It has been
removed: capturing the display steals focus and dismisses menus, which made the
G5 unusable to anyone sitting at it.

## ~~1b. Sessions died about a minute after the screen went still~~ (fixed)

Worth recording because it was invisible to every probe: they do not hold a
session open long enough.

Liveness in this protocol is driven by the **server**. Upstream's
`Connection::run` ticks every three seconds, sends a `TestDelay`, and drops the
peer if nothing has arrived for thirty (`src/server/connection.rs:281`). The
client only ever answers -- it never initiates one. This agent only echoed, so
once the screen stopped changing there were no frames, no test delays, and not
one byte in either direction. Measured three times running: last frame, then
57.4 seconds of complete silence, then the client dropped the session and
reconnected.

`TEST_DELAY_INTERVAL` in `src/session.rs` now drives it the way upstream does,
re-sending after ten seconds if an answer goes missing so a peer that ignores
them cannot wedge the timer. It also logs the round trip, which is the client's
latency readout as well.

## 1e. The C shims are built at the one optimisation level that miscompiles

Found by crashing the agent for a real user. A small static function doing
nothing but double arithmetic over file-scope statics -- no CoreGraphics call in
it -- faults with SIGBUS when built by `gcc10-bootstrap` for
`powerpc-apple-darwin`, and only at `-O2`:

```text
-O0 : ok      -O2          : Bus error
-O1 : ok      -O2 -maltivec: Bus error
-Os : ok
```

Reduced in `probes/` (the clickprobe/bisect pair, kept out of the tree since
they were throwaway): breaking the same statements apart with `printf` between
them makes it survive, which is the signature of a codegen bug rather than
anything in the source. `build.rs` compiles all three shims with
`.opt_level(2)`.

Bisected to a single pass. Of the obvious candidates, only one avoids it:

```text
-O2                        Bus error
-O2 -fno-store-merging     Bus error
-O2 -fno-strict-aliasing   Bus error
-O2 -fno-schedule-insns2   Bus error
-O2 -fno-tree-slp-vectorize Bus error
-O2 -fno-gcse              survives
```

So there *is* a narrower flag than dropping the file to `-O1`.

### The mechanism, disassembled at last (2026-08-06)

**A register is read on a path where it was never written.** Not a misaligned
access, which is what this section used to guess. That guess is now the *third*
wrong one, after a missing prototype and store merging — the section's own
warning was right, and the way to stop guessing was `-S`.

`probes/clipwatch.c` compiled both ways, and every mention of `r22` — which
holds `&cur`, i.e. `r1+1096`:

```text
-fno-gcse (works)          -O2 (crashes)
134: addi r22,r1,1096      209: mr   r3,r22     <- used
154: mr   r6,r22           235: mr   r10,r22    <- used
166: mr   r3,r22           248: mr   r4,r22     <- used
191: mr   r10,r22          271: addi r22,r1,1096   (block L18)
209: mr   r4,r22           280: addi r22,r1,1096   (block L32)
```

At `-O2` the address computation exists only in `L18` and `L32`, which are the
two *early-return* paths of `read_text` (`return -1` and `return 0`). The
success path does `CFDataGetBytes`, stores `out[len] = 0`, calls `CFRelease`,
and **falls through into `L4`**, which immediately does `mr r3,r22` and calls
`strcmp`. Nothing on that path ever writes r22, so `strcmp` receives whatever
the callee-saved register happened to hold. Here that was near zero:

```text
EXC_BAD_ACCESS (SIGBUS), KERN_PROTECTION_FAILURE at 0x0
0  libSystem.B.dylib  strcmp + 192
1  clipwatch          main + 572
```

With `-fno-gcse` there is a single definition in the entry block, dominating
every use, which is what the source means.

**How a real bug in the same file was ruled out.** `read_text` returned on two
paths without writing `out`, so the caller ran `strcmp` over an uninitialised
buffer — genuine undefined behaviour, and a tidy explanation for a crash inside
`strcmp`. It was wrong: fixing it changed nothing, and the rebuilt binary
produced a byte-identical crash at the same offsets with the same registers.
That is what turned the diagnosis back to the compiler and made the `-S` diff
worth doing. The UB was fixed anyway.

Neither the diagnosis nor the flag changes what ships — `NO_MISCOMPILE` has been
on every shim since the first instance — but it does settle what the flag is
protecting against, and it raises the value of the reference checks below:
this class of failure produces a wrong *pointer*, so it can just as easily
corrupt quietly as crash.

**It happened again, in a different file.** `probes/clipwatch.c` -- CoreFoundation
calls and string compares, no floating point at all -- was built at plain `-O2`
out of habit and died on its first loop iteration. Rebuilt with `-fno-gcse`,
nothing else changed, it ran for its full 240 ticks. So the miscompile is not
specific to double arithmetic over file-scope statics, which was the shape of
the first instance. It is broader than that, and the mechanism below explains
why the two look nothing alike.

Nothing currently shipping is known to be affected -- `convert_shim.c` is
checked byte-for-byte against the Rust reference on a real frame by
`--probe-display`, and the vpx and input shims have run for hours -- but the
next arithmetic-heavy shim is a coin toss. Worth doing:

- ~~Decide whether to add `-fno-gcse`~~ **Done, and it is free.** The
  converter measures 21 ms with it against 20 ms without, which is noise, so
  all three shims now carry it (`NO_MISCOMPILE` in `build.rs`). The worry that
  GCSE was worth keeping did not survive measuring it.
- ~~Still open: **why**~~ **Answered** — see the section above. GCSE sinks an
  address computation into some predecessors of a join block and not others,
  leaving a register read on a path that never wrote it. One `-S` diff settled
  what three rounds of reasoning had got wrong.
- **Verify C shims against a reference on the target**,
  the way the converter is. That check is what makes a miscompile survivable,
  and it is cheaper than understanding the compiler.

## ~~1d. Video is given up on for the whole session if it fails once~~ (closed 2026-08-06)

**Closed by decision, with the recovery done and the cause unfound.** Not closed
because it was explained — it was not. The reasoning: the retry turns the
symptom from "every peer gets no picture until someone restarts the agent" into
at most five seconds, and nine hours of trying could not make it happen again.
Chasing a cause that will not reproduce is worth less than being ready for it,
and the agent is now ready — see "How to pick this up" below for exactly what it
will print.

Reopen on the next real occurrence, which is when there will finally be
something to go on.

`Video::new` runs at login, and if `Capturer::new` fails the session logs
"serving input only" and never tries again. Seen for real: an agent that had
read the display fine at startup — its own banner said `display : 1920x1080` —
had `CGDisplayBaseAddress` return NULL eleven minutes later, and every peer that
connected afterwards got a working mouse and no picture until the process was
restarted. A freshly exec'd process on the same machine read the framebuffer
without trouble at the same moment, so whatever goes stale belongs to the
long-lived process rather than to the display.

**The recovery is done; the cause is not.** These are separate faults and only
one of them needed the cause:

- ~~**Retry**~~ **done.** A session that starts blind now rebuilds the whole
  video pipeline every `VIDEO_RETRY` (5 s) instead of serving input for ever,
  and `broken` -- set when the encoder cannot be rebuilt around a new screen
  size -- is covered by the same path. Same shape as `probe` tolerating a 16-bit
  colour mode. Whatever the cause turns out to be, the symptom is now bounded at
  five seconds rather than lasting until someone restarts the agent.
- **Find out what invalidates it.** Open. Two hypotheses have died, both
  recorded below because the *way* they died is the useful part.

### What has been ruled out, 2026-08-05

**Display sleep. No.** It was suspected on the strength of the failure being at
eleven minutes and `pmset` reporting `displaysleep 10` -- two numbers that look
alike, which is not evidence. Worse, the probe written to test it *could not*:
`fb-vigil` polling CoreGraphics every 15 s resets `HIDIdleTime` (visible in
`ioreg -c IOHIDSystem`), so the display stayed awake for 33 minutes and the
observation destroyed the thing observed. If you poll a display to ask whether
it is asleep, you have already answered no.

**Plain idleness. No.** The sharper reading of the report is that the agent read
the geometry at startup for its banner, made *no* CoreGraphics call while it
waited, and the first one after eleven minutes failed -- so an unused
window-server connection being torn down was the better theory.
`probes/fb-idle.c` reproduces exactly that sequence, doing nothing in between:
one `sleep`, no timer, no polling. **Clean at 2, 5, 11, 20 and 40 minutes**,
both with and without a mapping held from startup. 90, 180 and 420 running.

### The reported conditions do not reproduce, 2026-08-05 night

Tested against the real agent, with a real peer, in both launch configurations.
Every round: restart the agent for a clean t=0, touch nothing for N minutes --
no ssh, no probe, no connection -- then connect exactly once.

Nine hours, 15 agent rounds and 16 idle probes. **Not one failure.**

| scenario | waits (min) | result |
|---|---|---|
| cold start → idle → first peer, **launchd** | 11, 11 | clean |
| cold start → idle → first peer, **screen** | 11, 11, 11 | clean |
| **warm** (a session served first) → idle → peer, screen | 11, 11, 20, 30, 45, 60, 90, 150 | clean |
| `fb-idle`, pure idle in a C process, ± a held mapping | 2, 5, 11, 20, 40, 90, 180, 420 | clean |

Every agent round: 16 frames, 2 keyframes, first frame inside 0.85 s. And in
every one, a freshly exec'd process saw the same base address at the same
moment as the agent -- **`0xb0028000`, unchanged across nine hours and some
thirty process starts.** The divergence between an old process and a new one is
the entire substance of the original report, and nothing here produced a hint
of it.

**So the account in this item is not sufficient to produce the failure.** It was
written from a single occurrence, and something that was true then is not
recorded here. Do not re-run the table above; it is done.

The first version of the soak could not have found anything either, and the
reason is worth keeping: it connected every ten minutes *from t=0*, so the agent
was never idle from a cold start -- the connection at t=0 exercised
CoreGraphics and each one after kept the connection warm. Three healthy cycles
in a column is what made it visible. The same shape as `fb-vigil` keeping awake
the display it was asking about.

### How to pick this up

The retry means the symptom no longer costs a session, so the honest priority
now is *waiting for it to happen again* rather than hunting it. What the agent
will say when it does, which it would not have said before:

```text
capture unavailable: display 1 reads 0x0, 0 bpp, stride 0, base 0x0;
                     first tried 4210s ago, last succeeded 4208s ago
video unavailable: ... -- retrying every 5s
video is available again; this session now has a picture   (or not)
```

That is the whole state, plus the two numbers nobody could reconstruct
afterwards -- how long the process had been running, and whether capture had
*ever* worked in it. "Worked for an hour then stopped" and "never worked" have
different causes and used to produce the same message. Whether the retry
recovers it is itself the most useful single fact still missing.

### The tools, if it is worth chasing again

`probes/soak-video.sh`, on the host. `SOAK_LAUNCH` picks launchd or `screen`;
`SOAK_WARM=1` serves one session before the idle. It dumps the agent log, a
freshly exec'd process's view, HID idle and the process list the moment a
session starts blind, and the detector is the agent's own "video unavailable"
line rather than `frames=0` -- the retry above would otherwise hide a transient
failure behind a session that recovered.

Warm rounds were added because every cold round used a *virgin* agent that had
never built a `Capturer`, so none of them could catch a fault that needs one to
have been created and dropped first -- and the report never says no peer had
connected before, only that every peer *after* the failure got no picture. They
are clean too, so far.

### Variables still untried, if the warm rounds are clean too

* **A machine that has been up for days.** The G5 has been up a day and a half
  through all of this; the original incident's uptime is unknown.
* **Something else holding the display.** A screensaver, or another VNC-ish
  server. `CGDisplayCapture` by any process is the documented way to make
  `CGDisplayBaseAddress` useless to everyone else, and item 1 already records
  this agent doing that to itself.
* **Many more trials.** Five samples cannot distinguish "does not happen" from
  "happens one time in fifty".

Worth knowing that this is invisible to `--probe-display`, which is a fresh
process every time and so always gets a good mapping. Also that
`Capturer::new` now reports *which* query failed, with the whole state on one
line: a display query returning 0 used to be reported as "display is not 32 bits
per pixel", which reads as a colour-mode change rather than a dead connection.

## ~~1c. An unknown message from modern clients~~ (identified)

`d2 01 04 0a 02 0a 00` is **field 26, `PointerDeviceEvent`**, which the 1.1.8
oneof does not have -- it stops at 19, and current master runs to 32. Decoded
against `github.com/rustdesk/hbb_common`:

```text
field 26  PointerDeviceEvent
  field 1   TouchEvent
    field 1   TouchScaleUpdate {}      // scale defaults to 0
```

and the proto's own comment on that field says `0 means scale end`. So they are
**pinch-to-zoom gestures ending**, which is why they arrive in bursts while
someone is using the trackpad rather than on a timer. `PointerDeviceEvent` also
carries two-finger pan (`TouchPanStart` / `Update` / `End`).

Ignoring them is currently right, and not only out of laziness: **Leopard has no
pinch-zoom event to post.** Magnification gestures are 10.6+, so honouring a
scale update would mean inventing a mapping -- Cmd-plus and Cmd-minus, say --
which is application-specific and guesses at intent. The pan messages would
duplicate the scroll path that already works via `mask` kind 4.

Worth revisiting only if someone wants pinch-to-zoom badly enough to accept a
synthetic keyboard mapping for it.

## ~~2. LAN discovery~~ (done)

`src/lan.rs` answers the UDP broadcast on 21119 (`RENDEZVOUS_PORT + 3`) that
populates a client's local-network list, so the G5 no longer has to be reached
by typing its IP. `PeerDiscovery` is field 22 of `RendezvousMessage` and is
backported the same way `vp8s` and `pointer_device_event` were.

Verified by sending a real ping and decoding the reply:

```text
-> PeerDiscovery { cmd: "ping", id: "probe-client" }
<- cmd pong | id 4iv3930za | username admin | hostname PowerMacG5 | platform Mac OS
```

Its own thread, sharing three strings and nothing else: the session loop is a
tight millisecond budget and a socket that is silent for hours does not belong
in it.

`mac` is deliberately left empty. Upstream fills it so a client can wake the
machine over the network, which this agent cannot support in any case -- waking
it means it was off, and an agent that is off did not answer the broadcast.
Discovery itself does not read the field.

## 11. What the modern protocol survey turned up

Compared current `hbb_common` against the 1.1.8 proto this agent implements,
looking for things a modern client sends that we drop, or expects that we never
say. Recorded so the survey is not repeated.

**The version we report is load-bearing.** A modern client changes what it
sends based on it, and the gates in `src/common.rs` read it: at 1.2.4 the
refresh button switches to `Misc::refresh_video_display` (field 31), and at
1.4.5 the client may switch to *relative* mouse mode and send deltas instead of
absolute coordinates, which `decide_mouse` cannot handle. It used to come from
`CARGO_PKG_VERSION`; it is now pinned in `session.rs` with a test, because
bumping a crate version is an ordinary thing to do and would have broken the
mouse silently.

Checked and needing nothing:

* **`video_ack_required`** (LoginRequest field 9) -- the client never sets it,
  so there is no ack-based flow control to honour.
* **`VideoFrame.display`** and **`SwitchDisplay.cursor_embedded`** -- both
  default to what is already true here (display 0, cursor not drawn into the
  frame, since it is a hardware overlay).
* **`PeerInfo.features` / `encoding` / `resolutions`** -- unset, and the client
  copes: it decodes our VP8 without being told we can produce it.

## 11a. What claiming 1.2.4 turns on, one gate at a time

`Misc::refresh_video_display` is the gate everyone notices, but it is not the
only thing keyed to 1.2.4, and the version could not be raised until each of the
others had been looked at. Recorded so the next rung does not repeat it. Sources
are the client's `src/common.rs`, `src/ui_session_interface.rs`, `src/flutter.rs`
and `flutter/lib/common/widgets/toolbar.dart` at `ef3a575`.

**Implemented:** the refresh button (`refresh_video_display`, field 31, an int32
display index) and the per-display refresh the client sends when switching
displays. Both land in the same handler; the index is logged and ignored,
because there is one display. `refresh_video` (field 10) is still handled too --
a sciter build sends it at every version.

**Inert, each for its own reason** -- none of these can fire against this agent:

| what 1.2.4 unlocks | why nothing happens |
|---|---|
| `change_resolution` (24) becomes `change_display_resolution` (36) | we handle neither, and the menu needs `PeerInfo.resolutions`, which we do not send |
| renderer keys frames by `VideoFrame.display` instead of taking the first session | 1.1.8 has no such field, so it arrives as 0, and the client registers display 0 |
| "follow remote cursor" and "follow remote window focus" toggles | both also require `pi.displays.length > 1` |
| file copy and paste | requires `platformAdditions[has_file_clipboard]`, which we do not set |
| "true color (4:4:4)" toggle | requires the codec to be AV1 or VP9; we send VP8 |
| `toggle_privacy_mode` (33) | needs the user's `privacy-mode` toggle, which needs `PeerInfo.features.privacy_mode`, which we do not set |
| 1.2.2, crossed on the way: the custom-quality slider's maximum widens | we read our bitrate from config and only log the peer's quality options |

One upstream oddity found on the way and worth not being confused by:
`is_support_screenshot(&str)` in the client's `common.rs` delegates to
`is_support_multi_ui_session_num`, i.e. to the 1.2.4 gate rather than the 1.4.0
one its `_num` twin uses. It has no callers, so it is dead rather than dangerous.

**The refresh path is now measured, not merely plumbed.** `probe_client` sends
the field a 1.2.4+ client sends and times the keyframe that follows, with
`Misc::refresh_video(false)` first as a control -- because the agent already
emits a keyframe every `KEYFRAME_INTERVAL` while the settle repaint finishes a
lap, so "a keyframe arrived afterwards" proves nothing without it:

```text
keyframes at : [0.81, 9.73] s
refresh_video(false)     -> no keyframe within 2.0s     (control: nothing arrives unasked)
refresh_video_display(0) -> keyframe after 0.77s        (and the agent logged the request)
```

Still unproven: **no `Misc` message from a real client has ever been logged**, so
the peer-options branch -- the one that starts sending cursor shapes when "show
remote cursor" is switched on -- has never actually fired.

## ~~11b. What the 1.4.5 rung turned out to be~~ (done)

Reading the client turned "medium, unscoped" into three pieces, of which the
middle one was the surprise. All three are now implemented; kept because the
mechanism is worth not re-deriving, and because §11c depends on the table.

**What relative mouse mode actually is.** `MouseEvent` has not changed: no new
field, no option message. The mode is signalled entirely in `mask`, whose low
three bits gain kind **5**, `MOUSE_TYPE_MOVE_RELATIVE` (`src/common.rs`), and `x`
/ `y` then carry dx / dy instead of a position. Upstream's
`input_service.rs` clamps each to ±10000 -- the client's own
`kMaxRelativeMouseDelta` -- and adds them to wherever the cursor is now. Three
further details that matter here:

* **Nothing else changes.** Kinds 1 and 2 (button down and up) never used the
  coordinates anyway, so buttons keep working untouched.
* **Absolute movement turns it off.** A kind 0 event implicitly ends relative
  mode, upstream and here; there is no state to unwind.
* **The client asks for nothing.** No handshake, no option, no acknowledgement:
  the mode is enabled purely because we claim 1.4.5, and the client is
  documented as the sole authority over it. It is also user-initiated (pointer
  lock, for games and 3D apps) and Flutter-only, so it is off unless someone
  turns it on.

The injection side was expected to be small, and mostly was: `MouseAction::MoveBy`
carrying a delta, the same buttons-held check kind 0 does so a relative drag is
still a drag, and no C at all -- `rd_cursor_pos` already existed. What was *not*
in the estimate is that it needs a bounds check. The design read the position
back from the system on every event on the reasoning that the window server
clamps the pointer at the edge, which would make it self-correcting; that was
checked on the machine rather than assumed, and it is false. `CGPostMouseEvent`
accepts a point off the display and `CGEventGetLocation` reports it back, so one
hard flick left the pointer at 5700,5320 on a 1920x1080 screen for good. See
`land_delta` and the standing check in `--probe-live`.

**The surprise: claiming 1.4.5 also unlocks a screenshot button.**
`is_support_screenshot_num` is `ver >= 1.4.0` and reads nothing else -- no
capability flag, no platform key -- so the menu item appears, and pressing it
sends `ScreenshotRequest` (`Message` field 29), which this proto did not have.
Refusing honestly would have been about twenty lines, since `ScreenshotResponse`
carries an error string (`msg`, "empty if success"); serving it properly was
chosen instead, and `src/png.rs` is the result. Both paths exist now: the refusal
is what a 16-bit colour mode or a dead capturer gets.

**Everything else between 1.2.4 and 1.4.5 is inert**, and for a consistent
reason: upstream learned to gate on capability rather than on version, so nearly
every feature added since needs a `PeerInfo` flag or a `platformAdditions` key
that we do not set. Checked one at a time:

| gate | what it unlocks | why nothing happens |
|---|---|---|
| 1.2.7 | mobile action menus (Back, Home, recents) | all three sites require `platform == Android`; we say "Mac OS" |
| 1.3.0 | multi-clipboard (`MultiClipboards`) | no clipboard is implemented at all -- see item 6 |
| 1.3.0, 1.3.3, 1.3.8, 1.4.2 | file rename, drag-and-drop, copy-paste, transfer resume | all inside a file-transfer session, which this agent does not serve; copy-paste additionally needs `platformAdditions[has_file_clipboard]` |
| 1.3.8 also | a mobile peer's back gesture switches to `MOUSE_BUTTON_BACK` | needs the peer to be Android, but it found a real defect anyway -- see §11c |
| 1.3.9 | remote print; view-camera wording | camera needs `PeerInfo.support_view_camera`; the version only chooses which error text appears |
| 1.4.1 | terminal wording | needs `PeerInfo.support_terminal`; same, only the error text |

## 11c. The top of the ladder

**We now report 1.4.5, and that is the last rung.** Every version gate in the
client was enumerated rather than sampled -- all of them, from both halves of the
source, by pulling out the literals:

```bash
grep -rhoE 'get_version_number\("[0-9.]+"\)' --include=*.rs src/
grep -rhoE "versionCmp\([^,]+, *['\"][0-9.]+['\"]\)" --include=*.dart flutter/lib/
grep -rhoE "kMinVersion[A-Za-z]* *= *'[0-9.]+'" --include=*.dart flutter/lib/
```

which is the whole set: 1.1.9, 1.1.10, 1.2.0, 1.2.2, 1.2.4, 1.2.7, 1.3.0, 1.3.3,
1.3.8, 1.3.9, 1.4.0, 1.4.1, 1.4.2, 1.4.5, 1.4.8. Only two above 1.2.4 needed
work -- 1.4.0's screenshot button and 1.4.5's relative mouse, both now
implemented -- and the rest are in the table in §11b.

**Above 1.4.5 there is exactly one gate, 1.4.8, and it cannot fire here.**
`allowDisplaySwitchInPrivacyMode` compares against it only on the Windows
branch; a macOS peer returns true at any version, and it is inside privacy mode,
which needs `PeerInfo.features`. So there is nothing left to earn, and a test in
`session.rs` now asserts the constant *equals* 1.4.5 rather than merely being at
least it -- if a newer client adds a gate, that assertion is the place to notice.

One thing worth keeping from the survey, because it was a real defect rather
than a gate: from **1.3.8** a mobile peer's back gesture sends `MOUSE_BUTTON_BACK`
(8) where it used to send right (2), and `decide_mouse` fell through to *left*
for any button it did not recognise -- a stray click on whatever the pointer was
over, recorded as held so every later move reported as a drag. Unreachable (every
route to that gesture needs the peer to be Android), but fixed: the three buttons
`CGPostMouseEvent` carries are matched and the rest ignored, as upstream does.

## 12. Rendezvous registration

**The server is up and proven, 2026-08-06.** That was the precondition for any
agent work: a self-hosted hbbs/hbbr pair reached by a known-good peer — an
Ubuntu 24.04 box running RustDesk 1.4.9 — over cellular, serving a full AV1
session. Until a stock client could do it there was no point writing agent code,
because any failure could have been either end. Deployment specifics
(hostnames, keys, container config) are configuration and deliberately not
recorded here.

Three faults had to be cleared, and the useful part is that they were
**independent**. Only the first was in the original diagnosis, and the third was
not a fault at all but a bad measurement:

| what was wrong | how it showed | what fixed it |
|---|---|---|
| hbbs in Docker **bridge** mode with published ports | every peer's source address read as the bridge gateway `172.18.0.1` | `network_mode: host` on both containers |
| the peer advertised a **LAN relay address** | `create_relay … relay_server: <lan ip>` then `deadline has elapsed` | a publicly resolvable relay name, via hbbs `-r` or the peer's own `relay-server` |
| NAT type measured **SYMMETRIC** | forces relay on every path, including LAN | artifact of `docker-proxy`; re-measured ASYMMETRIC once bridge mode was gone |

The first is worth understanding rather than just fixing, because it explains
why LAN worked throughout and only remote failed. hbbs decides "same intranet"
by comparing the two peers' source IPs **for equality** (`same_intranet` in
`rendezvous_server.rs`). Collapse every peer onto one gateway address and that
test is unconditionally true, so hbbs told every caller to expect a local
connection. On the LAN the local addresses peers report about themselves happen
to be correct, so the wrong verdict was harmless there and fatal everywhere
else.

The third is a caution about instruments. `docker-proxy` opens its own
connection to the container with a fresh ephemeral port each time, and the NAT
test is precisely a check that two connections from one local port arrive with
the same source port. It could not have returned anything but SYMMETRIC. The
same shape as `fb-vigil` in §1d keeping awake the display it was asking about.

### Websocket is not available, and that settles the transport

The client can collapse the whole protocol onto `wss://host/ws/id` and
`/ws/relay` behind an ordinary reverse proxy — `check_ws` in
`hbb_common/src/websocket.rs`, enabled by the `allow-websocket` option, with
`wss` chosen only when `api-server` starts with `https`. It works as far as the
handshake: a proxy fronting hbbs `:21118` and hbbr `:21119` returns `101` and
the client connects.

Then registration fails, every time, in about two milliseconds:

```text
Client handshake done.
unknown RegisterPkResponse
WebSocket protocol error: Connection reset without closing handshake
```

**The OSS server does not implement it.** `handle_tcp` — which is what the
websocket loop calls — answers `RegisterPk` with `NOT_SUPPORT` and returns
`false`, which closes the connection (`rendezvous_server.rs:577`). Real
registration exists only in the UDP handler. There is exactly one occurrence of
`NOT_SUPPORT` in the whole server: no flag, no env var, no build option. The
client's own tooltip says so — *"NOTE: RustDesk server OSS doesn't include this
feature"* — and the misleading log line is the client's, since `_ =>` catches
`ID_EXISTS`, `TOO_FREQUENT`, `INVALID_ID_FORMAT`, `NOT_SUPPORT` and
`SERVER_ERROR` alike.

This is the good outcome for the G5. Websocket would have meant a websocket
client **and** TLS on Mac OS X 10.5, against a five-crate dependency list with
no async runtime. The native protocol needs neither.

### Remote connections are relay-only, structurally

Not a tuning problem and not the symmetric-NAT guess. The rendezvous server sits
behind the same NAT as the peers it serves, so their registration traffic
hairpins at the router and never crosses the WAN — no external mapping is ever
created, and hbbs can only record what it sees, which is a private address. It
therefore cannot hand a remote caller anything reachable, and the punch always
fails over to relay. Measured: a direct listen, then relay 1.2 s later, then a
working session.

So the agent's `RequestRelay` path is the one that carries every off-network
peer, and `FetchLocalAddr` carries the LAN ones. **`PunchHoleRequest` NAT
traversal is not needed** — which is the bulk of the protocol's complexity, and
what this item guessed from the start for the wrong reason.

### ~~The work~~ (done, 2026-08-06, and verified against the real server)

`src/rendezvous.rs`. `RegisterPeer` and `RegisterPk` to udp 21116 every 15 s --
hbbs drops a peer after 30 -- and three ways a peer arrives, all ending at
`session::serve` with an ordinary `TcpStream`. No local `relay-server` setting
is needed: hbbs advertises one to peers that do not set their own, and
`get_relay_server` prefers the local option only when present.

Each of the four was exercised against the live server, from a Linux build --
registration is platform-independent, so none of it waited on a G5 deploy. The
controlling side was driven by a probe that speaks the protocol by hand, for the
same reason `probe_client` exists:

| path | evidence |
|---|---|
| registration | `registered, reachable as id ...` in 132 ms, and **hbbs itself** reports the id online, with a one-character-different id as the control |
| `FetchLocalAddr` (same subnet) | server relayed our address and our registered pk to the caller; caller dialled it; `caller connected from ...` |
| `PunchHole` (we choose a relay) | caller got `RelayResponse` naming our uuid and relay, with our pk substituted for the id |
| `RequestRelay` (caller chose one) | caller joined the same uuid at hbbr and **received the agent's 122-byte `signed_id` through the relay** |

Every one started `step 1: sending signed_id`, which is the point of the second
item below: those sessions are encrypted whether or not `--secure` was passed.

**Two deliberate divergences from upstream**, both in the module header:

* **No hole punching.** Answering `PunchHole` with a relay we initiate is what
  upstream itself does for a peer it judges unpunchable, and here that judgement
  is structural rather than a guess -- see above. It also skips a round trip the
  caller would otherwise spend waiting.
* **No port reuse in the `FetchLocalAddr` path.** Upstream advertises the local
  port of its connection to the server and rebinds it, because punching needs
  the NAT mapping that connection made. We do not punch, so the advertised port
  can be one of our own -- and the reuse *does not work* from std: it needs
  `SO_REUSEADDR` on both sockets, and std cannot set options on an outgoing
  `TcpStream`. Measured failing with `EADDRINUSE`, because our end of the
  just-closed connection is in `FIN_WAIT`, which `SO_REUSEADDR` does not cover.
  That was found by running the path, not by reading it.

### What only the G5 could find: `u128` is emulated, and gets endianness wrong

Everything above passed on Linux and on the first PowerPC build, and the
local-network path was still broken on the machine. `AddrMangle` is built on
`u128`; 32-bit PowerPC gcc has no `__int128`, so mrustc emits a software
`uint128_t` -- `struct { uint64_t lo, hi; }` -- and the byte swap that
`u128::to_le_bytes` owes a big-endian target does not survive it.

```text
rendezvous: local-network request from 0.0.252.92:44107      <- really 192.168.99.153
rendezvous: listening on 192.168.99.116:50974 for the caller
rendezvous: local-network connect back failed: the caller never connected
```

The agent decoded the caller's address as noise, **re-encoded that noise** into
`LocalAddr.socket_addr`, and the server duly sent its reply to an address that
does not exist -- so the caller was never told where to go, and the agent sat
listening for someone who could not come. From the client it looked like a peer
that simply would not connect.

The relay paths were unharmed, because they pass `socket_addr` through as
opaque bytes and never re-encode it; only the log line was wrong there. That is
why cellular worked while the LAN did not, which is the opposite of what one
would guess.

Fixed by doing the arithmetic as an explicit `(hi, lo)` pair of `u64`s -- the
intermediate needs 82 bits, so the width is real, but every operation is now
native on any target. **The round-trip test could never have caught this**: it
agrees with itself in any byte order. `mangle_matches_the_wire_format` pins it
against a vector computed independently, which is the test that would have.

Confirmed on the machine afterwards: the same probe that hung now gets
`CONNECTED -- the agent accepted us`.

### A trap worth not rediscovering: an API server breaks connecting *to* us

Not this agent's doing, and it will look exactly like the agent's fault. A
client that holds a **login token** -- which is what signing in to any API
server gives it -- takes this branch in the client's `client.rs`:

```rust
if !key.is_empty() && (!token.is_empty() || !switch_code.is_empty()) {
    secure_tcp(&mut socket, &key).await   // waits for the server's KeyExchange
```

`secure_tcp` waits for the rendezvous server to speak first, and **`KeyExchange`
appears nowhere in the OSS server's source**. So every outbound connection dies
after the read timeout with `Failed to secure tcp: deadline has elapsed`, to
*every* peer, while inbound ones keep working because those ride the UDP
mediator. Seen for real, and it cost an evening's confusion because the symptom
arrived at the same moment as this agent's first deployment.

The escape is for the client not to hold a token, rather than to change which
API server it is. Clearing `key` also skips the check, but the server then
refuses the client's punch requests with `LICENSE_MISMATCH`, so that is not a
way out.

**Still to do**, now that the G5 itself is done:

* **`licence_key`.** The 1.1.8 proto has no such field in `RequestRelay` (it is
  field 6, added later). hbbr only checks it when started with `-k`, and an
  unkeyed relay is the common self-hosted case -- but against a keyed one the
  relay refuses us and the session never starts. Backporting the field the way
  `PeerDiscovery` was, plus a key to put in it, is the fix. Worth knowing that
  **hbbs is keyed even with no `-k`**, because it auto-generates `id_ed25519`
  and uses the public half; hbbr is not, because it has no such fallback.

Two things in the current code were shaped by the server's absence and have
changed now that it exists.

**What discovery advertises.** `lan.rs` used to put our *IP address* in the
`PeerDiscovery` id unconditionally, because the client only connects directly
when the id is one (`client.rs`: `if is_ip_str(peer)`) and otherwise asks a
rendezvous server to resolve it -- and there was none, so advertising the id
made the machine appear in the list and then refuse to connect. It now
advertises whichever is true: `me.id` when registered, the address otherwise.
The id is the better answer where it works, since it survives a DHCP change and
reaches another subnet. The port guard moved behind the same condition: it only
constrains the address form, because a registered agent's listening port stops
mattering once the server arranges the connection.

**The direct-IP handshake.** `session.rs` runs unencrypted by default because a
client connecting by IP never starts the `signed_id`/`public_key` exchange --
see the module header. A peer arriving via rendezvous *does*, so `rendezvous`
sets `secure` per connection from the route the peer took rather than from the
flag, and `Identity` is `Clone` so each session can carry its own. `--secure`
now governs only the direct-IP listener. Every one of the four verified paths
above began `step 1: sending signed_id`, which is that working.

## 3. Audio (built, works, reverted -- it captures the wrong thing)

**The code exists and is proven; it is not in the tree.** Built 2026-08-07 and
reverted the same day at the user's call, for a reason no amount of polish
fixes: it captures the default *input* device, and what anyone actually wants is
the sound the G5 is playing. Line-in noise at 2 kbit/s is not worth a feature.

`git show 2f4fa3a4d` is the whole of it, and `git revert` of the revert brings
it back. Do that if a loopback driver ever appears -- see below.

What it did, measured against a real session under the LaunchAgent, alongside
working video and clipboard:

```text
audio format announced: 48000 Hz, 2 channels
first audio frame after 0.24s (3 bytes)
audio frames : 1470 (4410 bytes, 2.3 kbit/s)
```

48 kHz stereo, Opus restricted-low-delay, 10 ms frames -- 1470 frames in 15 s is
98/second, which is the cadence. `AudioFormat` is `Misc` field 8 and
`AudioFrame` is `Message` field 11, both already in the 1.1.8 proto.

### What would make it worth having

**A loopback driver, and no code change.** Mac OS X cannot capture its own
output -- ScreenCaptureKit is 12.3+, twelve years after this hardware -- and
upstream's macOS path has the same limitation and the same answer: a loopback
driver *becomes* the default input, and the capture already written picks it up.
Soundflower shipped PowerPC builds for 10.4/10.5. That is the one thing standing
between the reverted commit and a useful feature, and it is an install rather
than a patch.

### Two findings that outlive the code

**10.5's AUHAL will not resample.** It converts channels and sample format, but
a device sitting at a rate other than the one asked for makes every
`AudioUnitRender` fail rather than convert. The fix is to set the *device's*
rate first (`kAudioDevicePropertyNominalSampleRate`) and then ask the unit; the
PCM3052 in this machine offers 48 kHz natively. Anything else touching CoreAudio
here will hit this.

**The shim could not say why it was silent, and that cost the evening.** It
opened, reported 48 kHz stereo, and returned zero samples in five seconds -- not
silence, which is 48000 zeros a second, but nothing. The render error was
swallowed and nothing was counted, so "no audio" was one message for a unit that
never runs and a unit that runs and fails every render, which want opposite
investigations. Adding three counters -- callbacks, failed renders, last
`OSStatus` -- turned it into an answer in one build.

**This is the same instrumentation gap item 1d records for the capture path**,
repeated within a day of writing that down. The lesson evidently does not
transfer by being written once: a probe that reports *what happened* rather than
*that it failed* is worth building before it is needed, not after.

### If it is ever picked up again

`magnum-opus` was the plan and is not what was built. It carries a bindgen build
step, and every build-time crate has to be transpiled for the host by mrustc
before a line of the agent compiles; the API actually used is four functions.
`opus_shim.c` in the reverted commit calls them directly, with headers vendored
under `opus-include/` for the reason vpx's are.

Also 10.5-specific and easy to lose an hour to: `AudioComponentFindNext` is
10.6, so a HAL unit has to come from the Component Manager. Every example
written since 2009 uses the newer pair.

## ~~4. Keyboard beyond raw keycodes~~ (done)

`ControlKey`, `unicode` and `seq` are all handled, and modifiers become
`CGEventFlags`. The premise was also wrong in a way worth recording: `chr` is
not a raw keycode, it is a *character* — "test" arrives as 116,101,115,116, and
posting those as Mac virtual keycodes types PageUp, F9, Home, PageUp. The shim
maps characters back to keycodes with `UCKeyTranslate`, which follows the
machine's actual layout and covers all 95 printable ASCII characters.

Still open: verifying delivery end-to-end into an application. Keystrokes go
wherever the focus is, and a listen-only `CGEventTap` — the way to check this
without guessing — needs "Enable access for assistive devices" in Universal
Access, which is off. `--probe-keys` prints the character→keycode table as the
part that *can* be checked unattended.

## ~~5. Cursor shape~~ (done)

Confirmed early on: the pointer is a hardware overlay and is **not** in the
framebuffer -- a 100x100 patch of a captured frame centred on it holds exactly
one colour. So the messages really are the only way a viewer gets a pointer.

The real shape now comes from the window server through the private CGS calls
in `src/cursor_shim.c`, the same ones every pre-10.6 VNC server used. The
earlier note here said this was out of reach; it is not, it is merely
undocumented, and it works on 10.5.8:

```text
seed 1442 | 24x24 | hotspot (4,4) | depth 32, components 4, bits/component 8
```

Two things that had to be established rather than assumed:

* **Byte order is A,R,G,B**, like the framebuffer. Found by dumping a real
  cursor and counting: byte 0 was non-zero across 281 of 576 pixels -- the
  whole arrow silhouette -- while bytes 1 to 3 were non-zero on 76, the white
  interior alone. `CursorData` wants RGBA, so the channels rotate on the way
  out, and the result was rendered and looked at before it shipped.
* **`CGSCurrentCursorSeed` is what makes polling affordable.** It is one call
  and changes only when the shape does, so the session loop polls it every pass
  and fetches the image only when it has actually moved on. The seed doubles as
  the `CursorData` id, which is exactly what the client caches shapes by.

A shape that cannot be read falls back to the built-in arrow rather than to no
pointer at all.

## ~~6. Clipboard~~ (done, both directions, confirmed against a real client)

Text, both directions, working. The agent must be started from the LaunchAgent,
and the one command that does it works from an ordinary ssh login:

```
~/rustdesk-ctl            # deploy/agent-ctl.sh, installed on the G5
```

**`launchctl -S Aqua` is the whole trick, and it is not in Leopard's usage
text.** Plain `launchctl load` filters by the *caller's* session type, so from
ssh it finds a plist marked `LimitLoadToSessionType Aqua`, matches nothing, and
says `nothing found to load` — which is true and says nothing about what to do.
`-S Aqua` names the session to load *into*, and the job then starts inside the
GUI session with the window server and the pasteboard. `unload` needs it too, or
it reports `nothing found to unload` and leaves the agent running. This cost an
evening; it is one flag.

A dead end worth not repeating: an ssh login's `launchctl list` **does** contain
the GUI applications, with Carbon PSN labels like `[0x0-0xd70d7].com.apple.dock`.
That looks like proof of being in the GUI session and is not. `pbpaste`
succeeding is the test that distinguishes them.

Verified rather than inferred, since "no error in the log" is not evidence:

* **Mac → peer.** A separate application copied on the G5 (`pbcopy`, run in the
  Aqua session), the agent noticed and sent it, and `probe_client` received
  `"typed-on-the-g5-2136"` via `multi_clipboards`.
* **peer → Mac.** `probe_client` sent two markers, and `probes/clipwatch.c` —
  an independent process reading the real pasteboard — found each of them on it.
* **Both directions against a real client**, confirmed by the user.

`clipwatch` also settled the polling question. `PasteboardSynchronize` reports
`kPasteboardModified` correctly when *another* client writes, but **not on the
first synchronise in a process** — so whatever was already on the G5's clipboard
when a peer connected would never be offered. The first poll of each session now
ignores the flag and reads the text, which is what upstream does at session
start.

**The pasteboard needs the Aqua session, and capture is no guide to that.**
`CGDisplayBaseAddress` works perfectly over ssh, so it was reasonable to expect
the clipboard to as well. It does not: `PasteboardCreate` returns **-4960** from
an ssh login *and* from the detached `screen` that `build-ppc.sh deploy` uses.
`pbcopy` and `pbpaste` fail there too, which is what rules out the API choice —
the Scrap Manager or Cocoa would fail the same way, because it is the session.
Measured with `probes/pasteboard.c`; `probes/clipshim.c` then drives the real
shim at its production flags and confirms every call returns -1 rather than
crashing, which is the path that runs on a machine nobody has switched over.

`launchctl bsexec` into the Finder's session is the other way in and needs root,
which is not available here. So the switch is a human at the G5:

```
# in Terminal.app on the G5 itself -- an ssh session reaches a different launchd
ssh ppctiger 'screen -S rdagent -X quit'      # only one agent may hold 21118
launchctl load -w ~/Library/LaunchAgents/com.rustdesk.ppc-agent.plist
```

The plist is already installed on the G5 and `plutil -lint` passes. See its
header for why the two launch methods must not both be running.

**Two things about a modern client that the 1.1.8 proto hides**, both found
before writing code rather than after:

* **The message moved.** At 1.3.0+ the client sends `MultiClipboards` (field 28)
  to any non-iOS peer, not `Clipboard` (16). At the 1.4.5 we report, field 16
  never arrives from a desktop peer at all. Both are decoded.
* **The content is really zstd**, not the raw-block frames `zstd_frame.rs`
  writes — those exist because the cursor path only ever needs to *produce*
  one. Reading a peer's clipboard needs a real decompressor, so this links
  libzstd, which turns out to be on the G5 already (1.5.7 in `/opt/local`,
  beside the libraries the binary depends on regardless). Worth knowing for
  anything else that wants compression.

**Text only**, deliberately: a browser copy arrives as three entries (text, HTML,
RTF) and the text one is picked out. Images would mean converting RGBA and PNG
into pasteboard flavours both ways.

The loop is the part that needed care rather than the API. Writing a peer's
clipboard onto the Mac marks the pasteboard modified, so the next poll reads it
back, sends it to the peer, whose own sync applies it and sends it back for
ever. `clipboard::Sync` remembers the last text that crossed in either direction
and drops the echo; both directions are host-tested.

Still to do once someone has switched the launch method over:

- Confirm the round trip against a real client, in both directions.
- Decide whether to honour `OptionMessage.disable_clipboard`, which the agent
  currently logs and ignores. Cheap, and it is what upstream does.

## ~~7. Conversion speed~~ (done)

`argb_to_i420` was ~185 ms in Rust and is **20 ms** in `convert_shim.c`, with
`--probe-display` comparing the planes byte for byte against the Rust reference
on a real frame every time it runs. The same move was made again for the
screenshot packer: 557 ms to 17.

AltiVec was the next rung and is **not worth taking**: it would save ~10 ms of a
405 ms full-screen frame, 333 of which is the VRAM read. See
[`performance-plan.md`](performance-plan.md) §4 — there is no cheap lever left,
and half resolution was offered and turned down.

## 8. Multi-monitor (deferred indefinitely)

`PeerInfo` reports a single display and capture only reads the main one.

**Deferred by decision, 2026-08-05: there is no second monitor to plug into the
G5.** Not declined on merit — it would simply be built against nothing, and
every part of it (which display is current, `SwitchDisplay`, per-display
refresh) is the kind of thing this project has repeatedly found to be wrong in
ways only real hardware shows. Reopen if a monitor appears.

Worth knowing it is already handled where it costs nothing to handle: the
`refresh_video_display` index is logged and ignored because there is one
display, and the client's follow-cursor and follow-window toggles stay hidden
because they require `pi.displays.length > 1` (§11a).

The speculative "tiles as displays" idea in
[`performance-plan.md`](performance-plan.md) §9 shares machinery with this but
is not blocked by it — it declares tiles of one real screen.

## ~~9. Running as a service~~ (done, with the session question answered)

`deploy/com.rustdesk.ppc-agent.plist` runs it in the console ("Aqua") session,
loaded with `launchctl load -w` from Terminal.app on the G5 — an ssh session
reaches a different launchd and cannot do it.

The session question has a sharper answer than "capture requires a window-server
session". A *fully* detached agent — backgrounded with `&`, or nohup — cannot
reach the window server at all ("On-demand launch of the Window Server is
allowed for root user only"), reports a 0x0 display, and silently serves input
with no video. A detached `screen` session keeps enough of the login session
that capture keeps working after ssh closes, which is what `build-ppc.sh deploy`
now uses.

## 10. Upstream the mrustc fixes (held, not abandoned)

Branch `ppc-upstream` in the mrustc tree holds the two required fixes (semver
pre-release in `CARGO_PKG_VERSION`, and the Darwin/PowerPC union alignment cap),
both based on `upstream/master` and independently useful. `ppc-async-fixes` holds
five async fixes that this agent does not need but which move mrustc materially
closer to compiling tokio-era code.

**Not proposing them yet, by decision, 2026-08-05: there is no fully working
tree to propose them from.** A patch to someone else's compiler is a claim that
it works, and the honest version of that claim needs a build that stands on its
own rather than one wired to a particular G5 and a particular set of
`/opt/local` libraries. The fixes are small and stable; the cost of holding them
is nil, and the cost of sending them early is a maintainer's time and a
reputation for noise.
