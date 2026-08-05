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

## 1d. Video is given up on for the whole session if it fails once

`Video::new` runs at login, and if `Capturer::new` fails the session logs
"serving input only" and never tries again. Seen for real: an agent that had
read the display fine at startup — its own banner said `display : 1920x1080` —
had `CGDisplayBaseAddress` return NULL eleven minutes later, and every peer that
connected afterwards got a working mouse and no picture until the process was
restarted. A freshly exec'd process on the same machine read the framebuffer
without trouble at the same moment, so whatever goes stale belongs to the
long-lived process rather than to the display.

Two things to do, in order:

- **Retry.** `probe` already tolerates a 16-bit colour depth by pausing and
  picking up again by itself; a null base address deserves the same treatment
  rather than a dead session. Rebuild the `Capturer` every few seconds while
  `broken`.
- **Find out what invalidates it.** Suspect display sleep. `probes/fb-settle.c`
  and `fb-livecheck.c` are the shape of probe that would answer it: hold a
  mapping, let the display sleep, and see what the base address does.

Worth knowing that this is invisible to `--probe-display`, which is a fresh
process every time and so always gets a good mapping.

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

## 11b. The next rung, 1.4.5, now that it has been scoped

The remaining gate. Reading the client turned "medium, unscoped" into three
pieces, of which the middle one is the surprise.

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

So the injection side is small: a `MouseAction` carrying a delta, the same
buttons-held check kind 0 already does to make it a drag rather than a move, and
a shim call that reads the current position and adds. All host-testable.

**The surprise: claiming 1.4.5 also unlocks a screenshot button we cannot
serve.** `is_support_screenshot_num` is `ver >= 1.4.0` and reads nothing else --
no capability flag, no platform key -- so the menu item appears, and pressing it
sends `ScreenshotRequest` (`Message` field 29), which this proto does not have.
Answering is cheap even without implementing it: `ScreenshotResponse` carries an
error string (`msg`, "empty if success"), so refusing honestly is a backport and
about twenty lines, against a PNG encoder for doing it properly.

**Everything else between 1.2.4 and 1.4.5 is inert**, and for a consistent
reason: upstream learned to gate on capability rather than on version, so nearly
every feature added since needs a `PeerInfo` flag or a `platformAdditions` key
that we do not set. Checked one at a time:

| gate | what it unlocks | why nothing happens |
|---|---|---|
| 1.2.7 | mobile action menus (Back, Home, recents) | all three sites require `platform == Android`; we say "Mac OS" |
| 1.3.0 | multi-clipboard (`MultiClipboards`) | no clipboard is implemented at all -- see item 6 |
| 1.3.0, 1.3.3, 1.3.8, 1.4.2 | file rename, drag-and-drop, copy-paste, transfer resume | all inside a file-transfer session, which this agent does not serve; copy-paste additionally needs `platformAdditions[has_file_clipboard]` |
| 1.3.9 | remote print; view-camera wording | camera needs `PeerInfo.support_view_camera`; the version only chooses which error text appears |
| 1.4.1 | terminal wording | needs `PeerInfo.support_terminal`; same, only the error text |

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

## 6. Clipboard

Phase 2 by decision. `Clipboard` message exists in the proto; the Mac side is
`NSPasteboard`/`PasteboardCreate`.

## 7. Conversion speed

`argb_to_i420` is ~185 ms against 14 ms for hand-written C at `-O2`. Ranked
options in [`videoperformance.md`](videoperformance.md#5-conversion-cost): move
it into a C shim as the other hot paths already are, `get_unchecked` in the hot
loop, raising the C optimisation level, or AltiVec. Note capture (351 ms)
dominates, so this is worth less than it looks — see
[`performance-plan.md`](performance-plan.md) for where it sits against
everything else.

## 8. Multi-monitor

`PeerInfo` reports a single display and capture only reads the main one.

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

## 10. Upstream the mrustc fixes

Branch `ppc-upstream` in the mrustc tree holds the two required fixes (semver
pre-release in `CARGO_PKG_VERSION`, and the Darwin/PowerPC union alignment cap),
both based on `upstream/master` and independently useful. `ppc-async-fixes` holds
five async fixes that this agent does not need but which move mrustc materially
closer to compiling tokio-era code.
