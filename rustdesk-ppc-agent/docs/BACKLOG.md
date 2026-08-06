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

So there *is* a narrower flag than dropping the file to `-O1`. What has not
been established is the mechanism -- GCSE moves and combines loads, and
PowerPC faults on a misaligned wide access where x86 would not care, but that
is a plausible story rather than a disassembled one. Two guesses at the
mechanism have already been wrong (a missing prototype, then store merging),
so treat the pass name as the finding and the explanation as unfinished.

**It happened again, in a different file, and confirms the flag is doing real
work.** `probes/clipwatch.c` -- CoreFoundation calls and string compares, no
floating point at all -- was built at plain `-O2` out of habit and died on its
first loop iteration, leaving a crash report and one line of output. Rebuilt
with `-fno-gcse`, nothing else changed, it ran for its full 240 ticks. So the
miscompile is not specific to double arithmetic over file-scope statics, which
was the shape of the first instance; it is broader than that, and every shim
this project ships carries the flag for good reason.

Nothing currently shipping is known to be affected -- `convert_shim.c` is
checked byte-for-byte against the Rust reference on a real frame by
`--probe-display`, and the vpx and input shims have run for hours -- but the
next arithmetic-heavy shim is a coin toss. Worth doing:

- ~~Decide whether to add `-fno-gcse`~~ **Done, and it is free.** The
  converter measures 21 ms with it against 20 ms without, which is noise, so
  all three shims now carry it (`NO_MISCOMPILE` in `build.rs`). The worry that
  GCSE was worth keeping did not survive measuring it.
- Still open: **why**. The pass name is empirical; nobody has disassembled the
  faulting instruction. Worth doing only if a second miscompile appears, since
  the flag costs nothing and the check below catches the class rather than the
  instance.
- **Verify C shims against a reference on the target**,
  the way the converter is. That check is what makes a miscompile survivable,
  and it is cheaper than understanding the compiler.

## 1d. Video is given up on for the whole session if it fails once (recovery done, cause open)

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

| scenario | rounds | result |
|---|---|---|
| cold start → 11 min → first peer, **launchd** | 2 | clean, 16 frames each |
| cold start → 11 min → first peer, **screen** | 3 | clean, 16 frames each |
| **warm** (one session served first) → 11 min → peer, screen | 2 | clean |
| warm → 20 min → peer, screen | 1 | clean |
| warm → 30 min → peer, screen | 1 | clean |
| `fb-idle`, pure idle in a C process | 7 waits: 2, 5, 11, 20, 40, 90, 180 min | clean, with and without a held mapping |

Every round reported 16 frames, 2 keyframes, and a fresh process seeing the same
good base address (`0xb0028000`) at the same moment -- which is the comparison
the original report turns on, and it never diverged once.

**So the account in this item is not sufficient to produce the failure.** It was
written from a single occurrence, and something that was true then is not
recorded here. Do not re-run the table above; it is done.

Longer warm waits (45, 60, 90, 150) and `fb-idle` at 420 minutes were still
running when this was written; anything they turn up belongs in this table.

The first version of the soak could not have found anything either, and the
reason is worth keeping: it connected every ten minutes *from t=0*, so the agent
was never idle from a cold start -- the connection at t=0 exercised
CoreGraphics and each one after kept the connection warm. Three healthy cycles
in a column is what made it visible. The same shape as `fb-vigil` keeping awake
the display it was asking about.

### What is running now

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

Planned: a private, self-hosted rendezvous server. Two things in the current
code are shaped by its absence and should change when it arrives.

**What discovery advertises.** `lan.rs` puts our *IP address* in the
`PeerDiscovery` id, because the client only connects directly when the id is
one (`client.rs`: `if is_ip_str(peer)`) and otherwise asks a rendezvous server
to resolve it. With a server registered, `me.id` becomes the better answer: an
id survives a DHCP change and works from another subnet. The line is marked.

**The direct-IP handshake.** `session.rs` runs unencrypted by default because a
client connecting by IP never starts the `signed_id`/`public_key` exchange --
see the module header. A peer arriving via rendezvous *does*, which is what
`--secure` already implements, so that path exists and is tested but is not the
default.

The registration itself is the work: `RegisterPeer` and `RegisterPk` to udp
21116, a heartbeat to stay listed, and answering `PunchHoleRequest` /
`FetchLocalAddr` by connecting back. A private server on a LAN can skip most of
the NAT traversal, which is the bulk of the protocol's complexity.

## 3. Audio

A phase-1 goal that is not started. `libopus.a` is already built and on the G5;
`magnum-opus` ships a checked-in `src/opus_ffi.rs`, so its bindgen build step can
be script-overridden away. Capture side is CoreAudio AudioUnit (10.4+). Frames go
in `AudioFrame { bytes data = 1 }`.

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
