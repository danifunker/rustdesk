# Backlog

Ordered roughly by what unblocks the most. Anything already measured links to
[`videoperformance.md`](videoperformance.md) rather than repeating numbers.

---

## 1. Change detection doesn't fire — capture may be stale

**The blocking unknown.** A session receives the initial keyframe and then
nothing. The screen was changed mid-session (AppleScript opened TextEdit,
confirmed by its reply) and no further frames followed.

Two candidate causes, needing very different fixes:

- **`CGDisplayBaseAddress` is not live.** Under Quartz Extreme the WindowServer
  composites on the GPU, and the pointer may hand back a main-memory buffer that
  no longer reflects the screen. If so the capture strategy needs replacing —
  the usual pre-10.6 alternative is an OpenGL readback (`CGLCreateContext` with
  a full-screen pixel format, then `glReadPixels`). Significant rework.
- **The dirty-band sampling is too sparse** and missed it. Then it is just
  tuning `PROBE_ROW_STEP` and the in-row stride in `src/capture.rs`.

**Cheapest discriminator:** run `--probe-display` twice with a visible change in
between and see whether `first px` or the dirty-band count moves. If they are
identical across a real change, it is the first cause.

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
