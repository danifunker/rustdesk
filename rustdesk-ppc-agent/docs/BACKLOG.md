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

## 2. LAN discovery

RustDesk clients find machines on the local network by UDP broadcast, so the G5
will not appear in that list. Implementable and self-contained:

- Listen on UDP **21119** (`RENDEZVOUS_PORT + 3`).
- Parse `RendezvousMessage`; on `PeerDiscovery { cmd: "ping" }` reply with
  `cmd: "pong"` plus `mac`, `id`, `hostname`, `username`, `platform`.
- `PeerDiscovery` is **field 22** of `RendezvousMessage` and does not exist in
  the 1.1.8 proto — backport it the same way `vp8s` was:

```protobuf
message PeerDiscovery {
  string cmd = 1;  string mac = 2;      string id = 3;
  string username = 4;  string hostname = 5;
  string platform = 6;  string misc = 7;
}
```

We already generate `rendezvous.proto`, so this is a proto addition plus a small
blocking UDP responder — no async needed. Until it exists, connect by typing the
IP into the client's ID field (see `../README.md`).

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

## ~~5. Cursor shape~~ (position done, shape approximated)

Confirmed: the pointer is a hardware overlay and is **not** in the framebuffer —
a 100x100 patch of a captured frame centred on it holds exactly one colour. So
the messages really are the only way a viewer gets a pointer.

`CursorData` is now sent once at login and `CursorPosition` whenever it moves
(`src/cursor.rs`). The shape is a built-in arrow rather than the real one: on
10.5 the system-wide cursor image is only reachable through private CGS calls,
and `NSCursor` knows only the calling application's own cursor. So the pointer
is in the right place but keeps its arrow over text fields and resize edges.
Reading the true shape is the remaining work.

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
