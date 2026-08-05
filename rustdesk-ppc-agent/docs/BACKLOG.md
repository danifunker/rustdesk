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

## 4. Keyboard beyond raw keycodes

`input.rs` handles `KeyEvent::chr` only. `ControlKey` and `unicode` need a
keymap table mapping RustDesk's key enum to Mac virtual keycodes — a
table-building exercise rather than a platform one. Modifiers are not applied
either.

## 5. Cursor shape

Clients expect `CursorData`/`CursorPosition` to draw a remote cursor. Nothing is
sent, so the viewer shows no pointer. Note the hardware cursor is likely a GPU
overlay and *not* in the framebuffer, so it will not appear in captured frames
either — this is the only way the viewer gets one.

## 6. Clipboard

Phase 2 by decision. `Clipboard` message exists in the proto; the Mac side is
`NSPasteboard`/`PasteboardCreate`.

## 7. Conversion speed

`argb_to_i420` is ~180 ms against 14 ms for hand-written C at `-O2`. Ranked
options in [`videoperformance.md`](videoperformance.md#5-conversion-cost):
`get_unchecked` in the hot loop, raising the C optimisation level, or an AltiVec
shim. Note capture (347 ms) dominates, so this is worth less than it looks.

## 8. Multi-monitor

`PeerInfo` reports a single display and capture only reads the main one.

## 9. Running as a service

Deferred by decision. Needs a `launchd` plist and a decision about which session
the agent runs in — capture requires a window-server session, so it cannot be a
plain system daemon.

## 10. Upstream the mrustc fixes

Branch `ppc-upstream` in the mrustc tree holds the two required fixes (semver
pre-release in `CARGO_PKG_VERSION`, and the Darwin/PowerPC union alignment cap),
both based on `upstream/master` and independently useful. `ppc-async-fixes` holds
five async fixes that this agent does not need but which move mrustc materially
closer to compiling tokio-era code.
