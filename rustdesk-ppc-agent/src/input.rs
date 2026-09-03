//! Mouse and keyboard injection, over the C shim in `input_shim.c`.
//!
//! Everything goes through the shim because `CGPoint` is two doubles passed
//! **by value**, and on 32-bit PowerPC that is exactly where a naive
//! `extern "C"` declaration stops matching the calling convention. A direct FFI
//! delivered x correctly and left y as denormal garbage:
//!
//! ```text
//!   asked for     : 640, 360
//!   cursor after  : 640, 0.0000...805
//! ```
//!
//! Only scalars cross the boundary now; the struct is built and consumed in C.
//!
//! `MouseEvent.mask` packs two fields, matching `input_service.rs`:
//!   * low 3 bits — 0 move, 1 down, 2 up, 3 wheel
//!   * bits 3..   — 1 left, 2 right, 4 middle

#[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
use std::os::raw::c_double;
use std::os::raw::{c_int, c_uint};

use crate::message_proto::{
    key_event, pointer_device_event, touch_event, ControlKey, KeyEvent, KeyboardMode, MouseEvent,
    PointerDeviceEvent,
};

// The shim reverse-maps characters to keycodes with UCKeyTranslate, which lives
// in Carbon; CoreGraphics alone cannot do it on this OS (see `rd_key_char`).
#[cfg(target_os = "macos")]
#[link(name = "Carbon", kind = "framework")]
extern "C" {}
// IRIX and Solaris get the same interface from XTEST; see each tree's input_shim.c.

#[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
extern "C" {
    fn rd_mouse(ty: c_int, x: c_double, y: c_double, button: c_int);
    fn rd_mouse_here(ty: c_int, button: c_int);
    fn rd_key_char(cp: c_uint, down: c_int, flags: c_uint);
    fn rd_keycode_for_char(cp: c_uint, needs_shift: *mut c_int) -> c_int;
    fn rd_release_modifiers();
    fn rd_scroll(dy: c_int, dx: c_int, pixels: c_int);
    fn rd_key(keycode: c_int, down: c_int);
    fn rd_key_unicode(cp: c_uint, down: c_int);
    fn rd_key_with_flags(keycode: c_int, down: c_int, flags: c_uint);
    /// A keycode in the keyspace of the platform we *told the peer we are*,
    /// which is not the same as a Mac virtual keycode anywhere but the Mac.
    /// Both X11 ports report "Linux" and so both have one; see
    /// `KeyAction::PlatformKeycode` and `linux_keycodes.h`.
    #[cfg(any(target_os = "solaris", target_os = "irix"))]
    fn rd_key_platform(keycode: c_int, down: c_int);
    #[cfg(any(target_os = "solaris", target_os = "irix"))]
    fn rd_key_platform_with_flags(keycode: c_int, down: c_int, flags: c_uint);
    fn rd_cursor_pos(x: *mut c_double, y: *mut c_double);
}

/// Let go of every modifier key.
///
/// Worth doing when a session starts: a modifier left held -- by a client that
/// disconnected mid-shortcut, or by an earlier build that stamped flags onto
/// events instead of pressing keys -- corrupts everything afterwards. A stuck
/// Control is the worst of them, because a Control-click is a right-click here,
/// so left-clicking silently starts opening context menus.
#[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
pub fn release_modifiers() {
    unsafe { rd_release_modifiers() }
}

/// Which keycode a character would be typed as under the current layout, and
/// whether it needs shift. `None` means the layout cannot produce it and the
/// character falls back to unicode entry.
///
/// Diagnostic only, for `--probe-keys`: the mapping is otherwise invisible, and
/// the alternatives for checking it on this OS are all blocked (event taps need
/// an accessibility toggle, and typing into a window tests focus as much as it
/// tests the mapping).
#[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
pub fn keycode_for_char(cp: u32) -> Option<(i32, bool)> {
    let mut shift: c_int = 0;
    let code = unsafe { rd_keycode_for_char(cp, &mut shift) };
    if code < 0 {
        None
    } else {
        Some((code, shift != 0))
    }
}

/// Where a relative move lands: the current position plus the delta, held inside
/// the display.
///
/// Separate and pure because the clamp is the part that has to be right --
/// `MouseAction::MoveBy` records what happens without it -- and because a
/// display size arrives from CoreGraphics, which host tests do not have.
///
/// A position already outside the display is pulled back in rather than left
/// where it is, so a pointer lost by anything else recovers on the next event.
pub fn land_delta(x: f64, y: f64, dx: f64, dy: f64, width: f64, height: f64) -> (f64, f64) {
    // An empty display would make the bounds cross over; treat it as a single
    // pixel rather than returning something ordered the wrong way round.
    let (xmax, ymax) = ((width - 1.0).max(0.0), (height - 1.0).max(0.0));
    ((x + dx).max(0.0).min(xmax), (y + dy).max(0.0).min(ymax))
}

/// Where the system thinks the cursor is. Out-params, not a returned struct.
#[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
pub fn cursor_position() -> (f64, f64) {
    let (mut x, mut y) = (0.0f64, 0.0f64);
    unsafe { rd_cursor_pos(&mut x, &mut y) };
    (x, y)
}

/// Mac virtual keycodes, from `HIToolbox/Events.h`. Only the keys a remote
/// session actually needs; anything else falls back to unicode entry.
fn control_key_to_keycode(k: ControlKey) -> Option<c_int> {
    Some(match k {
        ControlKey::Return | ControlKey::NumpadEnter => 36,
        ControlKey::Tab => 48,
        ControlKey::Space => 49,
        ControlKey::Backspace => 51,
        ControlKey::Escape => 53,
        ControlKey::Delete => 117,
        ControlKey::Home => 115,
        ControlKey::End => 119,
        ControlKey::PageUp => 116,
        ControlKey::PageDown => 121,
        ControlKey::LeftArrow => 123,
        ControlKey::RightArrow => 124,
        ControlKey::DownArrow => 125,
        ControlKey::UpArrow => 126,
        ControlKey::Shift => 56,
        ControlKey::RShift => 60,
        ControlKey::Control => 59,
        ControlKey::RControl => 62,
        ControlKey::Alt => 58,
        ControlKey::RAlt => 61,
        ControlKey::Meta => 55,
        ControlKey::RWin => 54,
        ControlKey::CapsLock => 57,
        ControlKey::F1 => 122,
        ControlKey::F2 => 120,
        ControlKey::F3 => 99,
        ControlKey::F4 => 118,
        ControlKey::F5 => 96,
        ControlKey::F6 => 97,
        ControlKey::F7 => 98,
        ControlKey::F8 => 100,
        ControlKey::F9 => 101,
        ControlKey::F10 => 109,
        ControlKey::F11 => 103,
        ControlKey::F12 => 111,
        _ => return None,
    })
}

/// `CGEventFlags` bits for the modifiers a client sends alongside a key.
/// Protobuf wraps repeated enums, so these arrive as `ProtobufEnumOrUnknown`.
fn modifier_flags(mods: &[protobuf::ProtobufEnumOrUnknown<ControlKey>]) -> c_uint {
    let mut f: c_uint = 0;
    for m in mods {
        f |= match m.enum_value_or_default() {
            ControlKey::Shift | ControlKey::RShift => 0x0002_0000,
            ControlKey::Control | ControlKey::RControl => 0x0004_0000,
            ControlKey::Alt | ControlKey::RAlt => 0x0008_0000,
            ControlKey::Meta | ControlKey::RWin => 0x0010_0000,
            ControlKey::CapsLock => 0x0001_0000,
            _ => 0,
        };
    }
    f
}

/// What a `MouseEvent` means, decided before any FFI is involved.
///
/// Split out so the protocol handling can be tested on the host: the bugs this
/// code has had were all decisions (which button, whose coordinates), never the
/// posting itself.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum MouseAction {
    /// A plain move, or a drag when a button is held. `ty` is what the shim
    /// takes: 0 moved, 3 left-dragged, 4 right-dragged.
    MoveOrDrag { ty: i32, x: f64, y: f64 },
    /// A *relative* move: the deltas the client sends in relative mouse mode,
    /// which it uses for games and 3D applications once it has taken a pointer
    /// lock. `ty` is the same dial as `MoveOrDrag`, because dragging works there
    /// too.
    ///
    /// Applied against wherever the cursor is *now*, read back from the system on
    /// every event rather than accumulated here -- so a person using the real
    /// mouse on the G5 at the same moment is not fought over.
    ///
    /// The result is then clamped to the display by [`land_delta`], and that
    /// clamp is not belt-and-braces. It was written on the assumption that the
    /// window server clamps the pointer at the edge the way a physical mouse
    /// behaves, which would have made the read-back self-correcting and a bounds
    /// check unnecessary. `--probe-live` was extended to check that claim rather
    /// than rest on it, and it is false:
    ///
    /// ```text
    /// +5000,+5000   : cursor at 5700,5320
    /// ```
    ///
    /// `CGPostMouseEvent` accepts a point outside the display and
    /// `CGEventGetLocation` reports it back, so one hard flick in relative mode
    /// would have put the pointer in a coordinate space it never returned from --
    /// a mouse that simply stops working, with nothing in any log.
    MoveBy { ty: i32, dx: f64, dy: f64 },
    /// A press or release. `at_cursor` means the event carried no coordinates,
    /// so the position must come from the system rather than from the message.
    Button { down: bool, button: i32, at_cursor: bool, x: f64, y: f64 },
    /// Wheel movement, already in the sense CoreGraphics wants.
    /// `pixels` distinguishes a trackpad from a wheel: a wheel notch is a
    /// line, a two-finger swipe is a distance in pixels, and sending one as the
    /// other is either imperceptible or a whole page per twitch.
    Scroll { dx: i32, dy: i32, pixels: bool },
    /// A `mask` whose low bits name no event we handle.
    Ignore,
}

/// What a touch gesture from the peer amounts to here.
///
/// `PointerDeviceEvent` is field 26, backported from current upstream -- this
/// proto's oneof stops at 19. A modern client sends it during ordinary trackpad
/// use, and every one seen in a real session has been a `TouchScaleUpdate` with
/// scale 0, which upstream's proto documents as "scale end": the marker a
/// two-finger gesture emits when it finishes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TouchAction {
    /// A two-finger pan, which is a pixel scroll by another name.
    Scroll { dx: i32, dy: i32 },
    /// A pinch, as a delta scale factor x1000. **Nothing posts this yet**: a
    /// magnify event has no public constructor on any version of macOS, so
    /// honouring one means undocumented `CGEvent` fields. Recognised and logged
    /// so the values a real client sends are visible before anyone writes that.
    Zoom { scale: i32 },
    /// A gesture with nothing to do: a pan boundary, a pinch ending, or a
    /// variant this proto does not carry.
    Ignore,
}

/// What a `KeyEvent` means. See [`MouseAction`] for why this is separate.
#[derive(Debug, Clone, PartialEq)]
pub enum KeyAction {
    /// A Mac virtual keycode, which is what `control_key` resolves to.
    Keycode { code: i32, down: bool, flags: u32, then_up: bool },
    /// A character. The shim maps it back to a keycode through the current
    /// layout -- it is *not* a keycode itself, however much it looks like one.
    Char { cp: u32, down: bool, flags: u32, then_up: bool },
    /// Typed by unicode string, with no keycode behind it.
    Unicode { cp: u32, down: bool, then_up: bool },
    /// A whole string, each character pressed and released.
    Seq(String),
    /// A keycode in the keyspace of the platform reported to the peer.
    ///
    /// Distinct from `Keycode` because the two overlap and mean different
    /// things: `control_key` resolves to Mac virtual keycodes, while a client
    /// in Map mode sends whatever the platform we *claimed* uses. Mac 36 is
    /// Return; Linux 36 is also Return, but Mac 38 is `l` and Linux 38 is `a`.
    /// One number, two answers, so they cannot share a path.
    PlatformKeycode { code: i32, down: bool, flags: u32, then_up: bool },
    /// A control key with no keycode on this platform.
    Unmapped,
    /// No key field set at all.
    Ignore,
}

/// Largest relative-movement delta accepted on one event, in either axis.
///
/// Upstream's `input_service.rs` clamps to the same figure, and its comment says
/// it matches the client's own `kMaxRelativeMouseDelta`. Kept because it costs
/// nothing and the value arrives from the network: a delta of two billion would
/// otherwise be added to a screen coordinate and handed to CoreGraphics.
const MAX_RELATIVE_DELTA: i32 = 10_000;

pub struct Injector {
    /// Buttons currently held, so a move can be reported as the drag it is —
    /// posting a plain move mid-drag breaks selection and drag-and-drop.
    buttons_down: u8,
    /// Framebuffer pixels per peer pixel. See [`Injector::set_display_scale`].
    display_scale: f64,
}

impl Injector {
    pub fn new() -> Self {
        Self { buttons_down: 0, display_scale: 1.0 }
    }

    /// Tell the injector that the peer's coordinates are in a *downscaled*
    /// space, and by how much.
    ///
    /// The session tells the peer the display is the size of the frames it will
    /// receive, not the size of the framebuffer — see `Video::served_size` —
    /// so on IRIX at 1/2 a click the peer reports at (100, 100) belongs at
    /// (200, 200). Applied here rather than at the call site because
    /// `decide_mouse` is the pure, host-tested half of this module and a
    /// coordinate transform is exactly the kind of decision it exists to hold.
    ///
    /// Absolute positions and relative deltas both scale; scroll deltas do not,
    /// because a wheel notch is not a distance on the screen.
    pub fn set_display_scale(&mut self, scale: usize) {
        self.display_scale = scale.max(1) as f64;
    }

    /// Decide what a mouse event means, and track which buttons are held.
    ///
    /// `mask` packs two fields, matching `input_service.rs`: the low 3 bits are
    /// the kind (0 move, 1 down, 2 up, 3 wheel, **4 trackpad**) and the rest is
    /// the button (1 left, 2 right, 4 middle).
    pub fn decide_mouse(&mut self, ev: &MouseEvent) -> MouseAction {
        let sc = self.display_scale;
        let (x, y) = (ev.x as f64 * sc, ev.y as f64 * sc);
        let kind = ev.mask & 0x7;
        let button = ev.mask >> 3;
        // The three buttons `CGPostMouseEvent` carries, and nothing else.
        //
        // This used to fall through to left, which is a stray click on whatever
        // the pointer is over: the client's mask also has back (8) and forward
        // (16), and a mobile peer's back gesture sends back rather than right
        // from 1.3.8 onwards. Unreachable here for now -- every route to it
        // requires the *peer* to be Android -- but the fall-through was a trap
        // rather than a decision. Upstream's `input_service.rs` matches the three
        // it knows and ignores the rest, which is this.
        let btn_idx = match button {
            1 => Some(0), // left
            2 => Some(1), // right
            4 => Some(2), // middle
            _ => None,
        };
        // A press or release arrives with no coordinates at all — proto3 drops
        // zero-valued fields, so the whole message is `mask`. Clicking at the
        // (0, 0) that implies lands every click in the top-left corner; the
        // position has to come from the system, as it does upstream.
        let at_cursor = ev.x == 0 && ev.y == 0;
        match kind {
            0 => MouseAction::MoveOrDrag { ty: self.drag_type(), x, y },
            1 => match btn_idx {
                Some(b) => {
                    self.buttons_down |= button as u8;
                    MouseAction::Button { down: true, button: b, at_cursor, x, y }
                }
                None => MouseAction::Ignore,
            },
            2 => match btn_idx {
                Some(b) => {
                    self.buttons_down &= !(button as u8);
                    MouseAction::Button { down: false, button: b, at_cursor, x, y }
                }
                None => MouseAction::Ignore,
            },
            // Both axes, and **not negated**, which is a deliberate departure
            // from the 1.1.8 `input_service.rs` this agent otherwise mirrors.
            //
            // That code negates both axes on every platform except Windows,
            // and copying it put scrolling the wrong way round against a
            // modern client -- reported directly, and against VNC on the same
            // machine as the control, which scrolls correctly and does no such
            // negation. The negation dates from when the wire carried a
            // Windows-oriented sign; a client new enough to send trackpad
            // events (kind 4, absent from this proto entirely) evidently sends
            // deltas already in the sense the platform wants.
            //
            // Measured on the G5 to remove the guesswork: a positive `dy`
            // posted through `rd_scroll` scrolls a window *up*, and the client
            // sends a negative `dy` for the gesture that should scroll *down*.
            // Passing it through unchanged is therefore what agrees with both
            // the gesture and VNC.
            3 => MouseAction::Scroll { dx: ev.x, dy: ev.y, pixels: false },
            // Trackpad. **Not in the 1.1.8 protocol this agent was built
            // from**, so it fell through to `Ignore` and every two-finger
            // scroll a modern client sent was discarded -- 317 of them against
            // 6 clicks in one real session, which is what "scrolling doesn't
            // work" turned out to mean. The deltas are pixels rather than
            // notches, hence the flag.
            4 => MouseAction::Scroll { dx: ev.x, dy: ev.y, pixels: true },
            // Relative movement, kind 5, which a client sends only to a peer
            // claiming 1.4.5 or newer. `x` and `y` are deltas rather than a
            // position -- the same fields, a different meaning, which is why
            // claiming that version without this arm would have taken every
            // delta for an absolute coordinate and parked the pointer near the
            // top-left corner.
            //
            // Nothing else about the mode is signalled: no option message, no
            // handshake, and no `MouseEvent` field was added for it. A kind 0
            // event ends the mode implicitly, here as upstream, because absolute
            // movement simply resumes.
            5 => {
                let ty = self.drag_type();
                MouseAction::MoveBy {
                    ty,
                    dx: ev.x.clamp(-MAX_RELATIVE_DELTA, MAX_RELATIVE_DELTA) as f64 * sc,
                    dy: ev.y.clamp(-MAX_RELATIVE_DELTA, MAX_RELATIVE_DELTA) as f64 * sc,
                }
            }
            _ => MouseAction::Ignore,
        }
    }

    /// 0 moved, 3 left-dragged, 4 right-dragged: what the shim's `type` means
    /// once a button is held. Shared by absolute and relative movement, which
    /// differ in where they land and not in what they are.
    fn drag_type(&self) -> i32 {
        if self.buttons_down & 1 != 0 {
            3
        } else if self.buttons_down & 2 != 0 {
            4
        } else {
            0
        }
    }

    /// Decide what a touch gesture means.
    ///
    /// `PointerDeviceEvent` is field 26, backported from current upstream --
    /// this proto's oneof stops at 19. A modern client sends it during ordinary
    /// trackpad use, and every one seen so far has been a `TouchScaleUpdate`
    /// with scale 0, which that proto documents as "scale end": the marker a
    /// two-finger gesture emits when it finishes.
    ///
    /// Pure and free-standing so the mapping can be tested away from a Mac; the
    /// version gate lives at the call site, because *whether* to act on a
    /// gesture is a property of the system and *what it means* is not.
    pub fn decide_touch(ev: &PointerDeviceEvent) -> TouchAction {
        let touch = match &ev.union {
            Some(pointer_device_event::Union::touch_event(t)) => t,
            _ => return TouchAction::Ignore,
        };
        match &touch.union {
            // A pan is a scroll with a different name: the deltas are pixels,
            // and pass through unnegated for the same reason `mask` kind 4
            // does.
            Some(touch_event::Union::pan_update(p)) => {
                TouchAction::Scroll { dx: p.x, dy: p.y }
            }
            // Start and end carry an absolute position and no movement; acting
            // on them would scroll by wherever the fingers happened to land.
            Some(touch_event::Union::pan_start(_)) => TouchAction::Ignore,
            Some(touch_event::Union::pan_end(_)) => TouchAction::Ignore,
            // Delta scale factor x1000, and zero means the pinch ended.
            Some(touch_event::Union::scale_update(u)) if u.scale != 0 => {
                TouchAction::Zoom { scale: u.scale }
            }
            _ => TouchAction::Ignore,
        }
    }

    /// Decide what a key event means. `press` asks for a full down-and-up.
    ///
    /// **`chr` means two different things**, and `mode` is how the client says
    /// which. In Legacy it is a *character* -- "test" arrives as 116,101,115,116
    /// -- and has to go through `UCKeyTranslate` to reach a keycode. In Map the
    /// client has already translated to the peer's platform and sends a Mac
    /// virtual keycode, which needs posting and nothing else.
    ///
    /// Reading the field rather than assuming a mode matters even at the
    /// version this agent reports: the mode is a setting in the client's UI, so
    /// a peer can choose Map against any server. Treating that as a character
    /// would type gibberish -- the same class of bug as the original, which
    /// posted characters as though they were keycodes and produced PageUp, F9,
    /// Home, PageUp for "test".
    pub fn decide_key(&self, ev: &KeyEvent) -> KeyAction {
        let flags = modifier_flags(&ev.modifiers);
        let down = ev.down;
        let then_up = ev.press;
        let mapped = matches!(
            ev.mode.enum_value_or_default(),
            KeyboardMode::Map | KeyboardMode::Translate
        );
        match &ev.union {
            // Already a keycode -- but for the platform this agent claims to
            // be, which is only the same thing as a Mac keycode on the Mac.
            Some(key_event::Union::chr(c)) if mapped => {
                KeyAction::PlatformKeycode { code: *c as i32, down, flags, then_up }
            }
            Some(key_event::Union::control_key(ck)) => {
                match control_key_to_keycode(ck.enum_value_or_default()) {
                    Some(code) => KeyAction::Keycode { code, down, flags, then_up },
                    None => KeyAction::Unmapped,
                }
            }
            // A character, *not* a keycode: typing "test" arrives as
            // chr(116) chr(101) chr(115) chr(116). Treating those as Mac virtual
            // keycodes typed PageUp/F9/Home/PageUp, which looked exactly like
            // keyboard input never arriving.
            Some(key_event::Union::chr(c)) => KeyAction::Char { cp: *c, down, flags, then_up },
            Some(key_event::Union::unicode(u)) => KeyAction::Unicode { cp: *u, down, then_up },
            Some(key_event::Union::seq(s)) => KeyAction::Seq(s.clone()),
            None => KeyAction::Ignore,
        }
    }

    /// Act on a touch gesture, if this system has gestures at all.
    ///
    /// The gate is here rather than in `decide_touch` because it is a fact
    /// about the machine, not about the message: 10.5 has no magnify event to
    /// post and no reason to pretend, so a peer's gestures are dropped rather
    /// than approximated into something nobody asked for.
    #[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
    pub fn touch(&mut self, ev: &PointerDeviceEvent) {
        if !crate::sys::has_gesture_events() {
            return;
        }
        let action = Self::decide_touch(ev);
        log::debug!("touch -> {:?}", action);
        match action {
            TouchAction::Scroll { dx, dy } => unsafe { rd_scroll(dy, dx, 1) },
            TouchAction::Zoom { scale } => {
                log::debug!("pinch, delta scale {}/1000 -- no magnify event to post it through", scale)
            }
            TouchAction::Ignore => {}
        }
    }

    #[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
    pub fn mouse(&mut self, ev: &MouseEvent) {
        let action = self.decide_mouse(ev);
        log::debug!("mouse mask={:#x} -> {:?}", ev.mask, action);
        unsafe {
            match action {
                MouseAction::MoveOrDrag { ty, x, y } => rd_mouse(ty, x, y, 0),
                MouseAction::MoveBy { ty, dx, dy } => {
                    // Read the position back from the system every time rather
                    // than keeping a running one here, so a person moving the
                    // real mouse on the G5 at the same moment is not fought over.
                    let (x, y) = cursor_position();
                    // And the display size every time too: it can change
                    // mid-session, and a stale one would clamp to the old screen.
                    match (x >= 0.0 && y >= 0.0, crate::capture::display_size()) {
                        (true, Some((w, h))) => {
                            let (nx, ny) = land_delta(x, y, dx, dy, w as f64, h as f64);
                            rd_mouse(ty, nx, ny, 0);
                        }
                        // No position to be relative to, or no display to clamp
                        // against. Dropping the event is right: treating either
                        // failure as a zero would fling the pointer to a corner.
                        _ => log::debug!("relative move dropped: no cursor position or display"),
                    }
                }
                MouseAction::Button { down, button, at_cursor, x, y } => {
                    let ty = if down { 1 } else { 2 };
                    if at_cursor {
                        rd_mouse_here(ty, button);
                    } else {
                        rd_mouse(ty, x, y, button);
                    }
                }
                MouseAction::Scroll { dx, dy, pixels } => {
                    rd_scroll(dy, dx, if pixels { 1 } else { 0 })
                }
                MouseAction::Ignore => {}
            }
        }
    }

    #[cfg(any(target_os = "macos", target_os = "irix", target_os = "solaris"))]
    pub fn key(&mut self, ev: &KeyEvent) {
        let action = self.decide_key(ev);
        log::debug!("key down={} press={} -> {:?}", ev.down, ev.press, action);
        let d = |b: bool| if b { 1 } else { 0 };
        unsafe {
            match action {
                KeyAction::Keycode { code, down, flags, then_up } => {
                    if flags != 0 {
                        rd_key_with_flags(code, d(down), flags);
                    } else {
                        rd_key(code, d(down));
                    }
                    if then_up {
                        rd_key(code, 0);
                    }
                }
                KeyAction::PlatformKeycode { code, down, flags, then_up } => {
                    // Both X11 ports translate: each reports "Linux" while
                    // running on a keymap that is not a PC's -- Sun on one, SGI
                    // on the other -- so the incoming keycode means something
                    // else locally. On the Mac the reported platform *is* the
                    // Mac and a Map-mode keycode really is a Mac keycode, so
                    // that path is unchanged.
                    #[cfg(any(target_os = "solaris", target_os = "irix"))]
                    {
                        if flags != 0 {
                            rd_key_platform_with_flags(code, d(down), flags);
                        } else {
                            rd_key_platform(code, d(down));
                        }
                        if then_up {
                            rd_key_platform(code, 0);
                        }
                    }
                    #[cfg(not(any(target_os = "solaris", target_os = "irix")))]
                    {
                        if flags != 0 {
                            rd_key_with_flags(code, d(down), flags);
                        } else {
                            rd_key(code, d(down));
                        }
                        if then_up {
                            rd_key(code, 0);
                        }
                    }
                }
                KeyAction::Char { cp, down, flags, then_up } => {
                    rd_key_char(cp, d(down), flags);
                    if then_up {
                        rd_key_char(cp, 0, flags);
                    }
                }
                KeyAction::Unicode { cp, down, then_up } => {
                    rd_key_unicode(cp, d(down));
                    if then_up {
                        rd_key_unicode(cp, 0);
                    }
                }
                KeyAction::Seq(s) => {
                    for ch in s.chars() {
                        rd_key_unicode(ch as c_uint, 1);
                        rd_key_unicode(ch as c_uint, 0);
                    }
                }
                KeyAction::Unmapped | KeyAction::Ignore => {}
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message_proto::{
        TouchEvent, TouchPanEnd, TouchPanStart, TouchPanUpdate, TouchScaleUpdate,
    };
    use protobuf::ProtobufEnumOrUnknown;

    fn mouse(mask: i32, x: i32, y: i32) -> MouseEvent {
        let mut e = MouseEvent::new();
        e.mask = mask;
        e.x = x;
        e.y = y;
        e
    }

    fn key(u: key_event::Union, down: bool, press: bool, mods: &[ControlKey]) -> KeyEvent {
        let mut e = KeyEvent::new();
        e.down = down;
        e.press = press;
        e.union = Some(u);
        for m in mods {
            e.modifiers.push(ProtobufEnumOrUnknown::new(*m));
        }
        e
    }

    const DOWN: i32 = 1;
    const UP: i32 = 2;
    const WHEEL: i32 = 3;
    /// `MOUSE_TYPE_MOVE_RELATIVE` in the client's `src/common.rs`.
    const RELATIVE: i32 = 5;
    const LEFT: i32 = 1 << 3;
    const RIGHT: i32 = 2 << 3;
    const MIDDLE: i32 = 4 << 3;

    // -- the bug that put every click in the top-left corner ------------------

    /// A press carries no coordinates: proto3 omits zero fields, so the message
    /// is nothing but `mask`. It must be posted where the cursor already is.
    #[test]
    fn a_press_without_coordinates_clicks_at_the_cursor() {
        let mut inj = Injector::new();
        match inj.decide_mouse(&mouse(LEFT | DOWN, 0, 0)) {
            MouseAction::Button { at_cursor, down, button, .. } => {
                assert!(at_cursor, "a positionless press must not be taken as (0, 0)");
                assert!(down);
                assert_eq!(button, 0);
            }
            other => panic!("expected a button press, got {:?}", other),
        }
    }

    /// A press that does carry coordinates is posted there, not at the cursor.
    #[test]
    fn a_press_with_coordinates_uses_them() {
        let mut inj = Injector::new();
        match inj.decide_mouse(&mouse(LEFT | DOWN, 640, 360)) {
            MouseAction::Button { at_cursor, x, y, .. } => {
                assert!(!at_cursor);
                assert_eq!((x, y), (640.0, 360.0));
            }
            other => panic!("expected a button press, got {:?}", other),
        }
    }

    #[test]
    fn releases_are_positionless_too() {
        let mut inj = Injector::new();
        inj.decide_mouse(&mouse(LEFT | DOWN, 0, 0));
        match inj.decide_mouse(&mouse(LEFT | UP, 0, 0)) {
            MouseAction::Button { at_cursor, down, .. } => {
                assert!(at_cursor);
                assert!(!down);
            }
            other => panic!("expected a release, got {:?}", other),
        }
    }

    // -- button and kind decoding --------------------------------------------

    #[test]
    fn each_button_maps_to_its_own_index() {
        let mut inj = Injector::new();
        let idx = |inj: &mut Injector, mask| match inj.decide_mouse(&mouse(mask, 5, 5)) {
            MouseAction::Button { button, .. } => button,
            other => panic!("expected a button, got {:?}", other),
        };
        assert_eq!(idx(&mut inj, LEFT | DOWN), 0);
        assert_eq!(idx(&mut inj, RIGHT | DOWN), 1);
        assert_eq!(idx(&mut inj, MIDDLE | DOWN), 2);
    }

    /// Back and forward exist in the client's mask and have no equivalent here:
    /// A downscaled session hands the injector peer coordinates in the
    /// *served* space, not the framebuffer's. Getting this backwards puts every
    /// click at a quarter of the distance from the top-left corner, which looks
    /// like a broken pointer rather than a wrong constant.
    #[test]
    fn peer_coordinates_scale_back_up_to_the_framebuffer() {
        let mut inj = Injector::new();
        inj.set_display_scale(2);
        match inj.decide_mouse(&mouse(0, 100, 80)) {
            MouseAction::MoveOrDrag { x, y, .. } => assert_eq!((x, y), (200.0, 160.0)),
            other => panic!("{:?}", other),
        }
        // A press carries coordinates too, and they are in the same space.
        match inj.decide_mouse(&mouse(LEFT | DOWN, 10, 20)) {
            MouseAction::Button { x, y, .. } => assert_eq!((x, y), (20.0, 40.0)),
            other => panic!("{:?}", other),
        }
    }

    /// Relative deltas are a distance on the screen and scale with it. Scroll
    /// deltas are not: a wheel notch is a line, not a length, so scaling one
    /// would make the wheel jump further on a downscaled session for no reason.
    #[test]
    fn relative_deltas_scale_but_scroll_does_not() {
        let mut inj = Injector::new();
        inj.set_display_scale(4);
        match inj.decide_mouse(&mouse(5, 3, -7)) {
            MouseAction::MoveBy { dx, dy, .. } => assert_eq!((dx, dy), (12.0, -28.0)),
            other => panic!("{:?}", other),
        }
        match inj.decide_mouse(&mouse(3, 0, -1)) {
            MouseAction::Scroll { dx, dy, pixels } => {
                assert_eq!((dx, dy, pixels), (0, -1, false))
            }
            other => panic!("{:?}", other),
        }
    }

    /// The default has to be 1, or every platform that never calls the setter
    /// silently doubles its own coordinates.
    #[test]
    fn an_unscaled_session_is_the_default() {
        let mut inj = Injector::new();
        match inj.decide_mouse(&mouse(0, 100, 80)) {
            MouseAction::MoveOrDrag { x, y, .. } => assert_eq!((x, y), (100.0, 80.0)),
            other => panic!("{:?}", other),
        }
    }

    /// `CGPostMouseEvent` carries three buttons. They used to fall through to
    /// left, which is a stray click on whatever the pointer is over -- and from
    /// 1.3.8 a mobile peer's back gesture sends back rather than right.
    #[test]
    fn a_button_this_platform_does_not_have_is_ignored_not_treated_as_left() {
        let mut inj = Injector::new();
        for (name, bits) in [("back", 8i32), ("forward", 16)] {
            assert_eq!(
                inj.decide_mouse(&mouse((bits << 3) | DOWN, 5, 5)),
                MouseAction::Ignore,
                "a {} press must not become a left click",
                name
            );
            assert_eq!(
                inj.decide_mouse(&mouse((bits << 3) | UP, 5, 5)),
                MouseAction::Ignore,
                "a {} release must not become a left click",
                name
            );
        }
        // And it must not have been recorded as held, or every later move would
        // be reported as a drag.
        assert_eq!(
            inj.decide_mouse(&mouse(0, 10, 10)),
            MouseAction::MoveOrDrag { ty: 0, x: 10.0, y: 10.0 }
        );
    }

    /// Wheel events carry both axes and are inverted relative to the wire,
    /// matching upstream's non-Windows path.
    /// Build a PointerDeviceEvent carrying one touch variant.
    fn touch(u: touch_event::Union) -> PointerDeviceEvent {
        let mut t = TouchEvent::new();
        t.union = Some(u);
        let mut p = PointerDeviceEvent::new();
        p.union = Some(pointer_device_event::Union::touch_event(t));
        p
    }

    #[test]
    fn a_two_finger_pan_is_a_scroll() {
        let mut u = TouchPanUpdate::new();
        u.x = 4;
        u.y = -9;
        assert_eq!(
            Injector::decide_touch(&touch(touch_event::Union::pan_update(u))),
            TouchAction::Scroll { dx: 4, dy: -9 }
        );
    }

    /// Start and end carry an absolute position, not a movement. Scrolling by
    /// it would jump by wherever the fingers landed.
    #[test]
    fn pan_start_and_end_move_nothing() {
        let mut s = TouchPanStart::new();
        s.x = 700;
        s.y = 400;
        assert_eq!(
            Injector::decide_touch(&touch(touch_event::Union::pan_start(s))),
            TouchAction::Ignore
        );
        let mut e = TouchPanEnd::new();
        e.x = 700;
        e.y = 400;
        assert_eq!(
            Injector::decide_touch(&touch(touch_event::Union::pan_end(e))),
            TouchAction::Ignore
        );
    }

    /// The one every real client has actually sent: scale 0, which the upstream
    /// proto documents as the end of a pinch rather than a zoom of nothing.
    #[test]
    fn a_zero_scale_is_the_end_of_a_pinch_not_a_zoom() {
        let mut z = TouchScaleUpdate::new();
        z.scale = 0;
        assert_eq!(
            Injector::decide_touch(&touch(touch_event::Union::scale_update(z))),
            TouchAction::Ignore
        );
        let mut z = TouchScaleUpdate::new();
        z.scale = 120;
        assert_eq!(
            Injector::decide_touch(&touch(touch_event::Union::scale_update(z))),
            TouchAction::Zoom { scale: 120 }
        );
    }

    #[test]
    fn an_empty_pointer_event_is_ignored() {
        assert_eq!(
            Injector::decide_touch(&PointerDeviceEvent::new()),
            TouchAction::Ignore
        );
    }

    #[test]
    fn a_wheel_event_carries_both_axes_unchanged() {
        let mut inj = Injector::new();
        assert_eq!(
            inj.decide_mouse(&mouse(WHEEL, 0, -3)),
            MouseAction::Scroll { dx: 0, dy: -3, pixels: false }
        );
        assert_eq!(
            inj.decide_mouse(&mouse(WHEEL, 5, 0)),
            MouseAction::Scroll { dx: 5, dy: 0, pixels: false },
            "a sideways swipe must not be dropped"
        );
        assert_eq!(
            inj.decide_mouse(&mouse(WHEEL, -2, 4)),
            MouseAction::Scroll { dx: -2, dy: 4, pixels: false }
        );

        // A trackpad swipe is the same decision in pixels, and must not be
        // dropped: it was, for every modern client, until this was added.
        const TRACKPAD: i32 = 4;
        assert_eq!(
            inj.decide_mouse(&mouse(TRACKPAD, -2, 4)),
            MouseAction::Scroll { dx: -2, dy: 4, pixels: true }
        );
        assert_ne!(inj.decide_mouse(&mouse(TRACKPAD, 0, 5)), MouseAction::Ignore);
    }

    #[test]
    fn an_unknown_kind_is_ignored_rather_than_guessed() {
        let mut inj = Injector::new();
        assert_eq!(inj.decide_mouse(&mouse(7, 1, 2)), MouseAction::Ignore);
    }

    // -- relative movement, which is the same two fields meaning something else --

    /// Kind 5 carries deltas, not a position. Taking one for the other is the
    /// failure the 1.4.5 gate exists to prevent: every relative event would land
    /// the pointer a few pixels from the top-left corner instead of moving it.
    #[test]
    fn kind_five_is_a_delta_and_not_a_position() {
        let mut inj = Injector::new();
        assert_eq!(
            inj.decide_mouse(&mouse(RELATIVE, 7, -3)),
            MouseAction::MoveBy { ty: 0, dx: 7.0, dy: -3.0 }
        );
        // A zero delta is a real event -- it is the marker the client sends to
        // announce the mode -- and must not be confused with the positionless
        // press that `at_cursor` exists for.
        assert_eq!(
            inj.decide_mouse(&mouse(RELATIVE, 0, 0)),
            MouseAction::MoveBy { ty: 0, dx: 0.0, dy: 0.0 }
        );
    }

    /// Relative movement drags exactly as absolute movement does; the buttons
    /// never carried coordinates, so nothing about them changes with the mode.
    #[test]
    fn a_relative_move_while_a_button_is_held_is_also_a_drag() {
        let mut inj = Injector::new();
        inj.decide_mouse(&mouse(LEFT | DOWN, 0, 0));
        assert_eq!(
            inj.decide_mouse(&mouse(RELATIVE, 4, 4)),
            MouseAction::MoveBy { ty: 3, dx: 4.0, dy: 4.0 }
        );
        inj.decide_mouse(&mouse(RIGHT | DOWN, 0, 0));
        inj.decide_mouse(&mouse(LEFT | UP, 0, 0));
        assert_eq!(
            inj.decide_mouse(&mouse(RELATIVE, -1, 0)),
            MouseAction::MoveBy { ty: 4, dx: -1.0, dy: 0.0 }
        );
    }

    /// The delta comes off the network and is added to a screen coordinate, so
    /// it is clamped the way upstream clamps it.
    #[test]
    fn an_absurd_delta_is_clamped_rather_than_added() {
        let mut inj = Injector::new();
        assert_eq!(
            inj.decide_mouse(&mouse(RELATIVE, i32::MAX, i32::MIN)),
            MouseAction::MoveBy {
                ty: 0,
                dx: MAX_RELATIVE_DELTA as f64,
                dy: -MAX_RELATIVE_DELTA as f64,
            }
        );
        // And a delta inside the limit is untouched, including at the edge.
        assert_eq!(
            inj.decide_mouse(&mouse(RELATIVE, MAX_RELATIVE_DELTA, -MAX_RELATIVE_DELTA)),
            MouseAction::MoveBy {
                ty: 0,
                dx: MAX_RELATIVE_DELTA as f64,
                dy: -MAX_RELATIVE_DELTA as f64,
            }
        );
    }

    /// The clamp that `--probe-live` showed was needed. `CGPostMouseEvent` takes
    /// a point outside the display and `CGEventGetLocation` reports it straight
    /// back, so without this a single hard flick sent the pointer to 5700,5320
    /// on a 1920x1080 screen and it never came back.
    #[test]
    fn a_relative_move_cannot_push_the_pointer_off_the_display() {
        // Ordinary movement is untouched.
        assert_eq!(land_delta(640.0, 360.0, 60.0, -40.0, 1920.0, 1080.0), (700.0, 320.0));
        // Each edge holds, at the last real pixel rather than one past it.
        assert_eq!(land_delta(1900.0, 1070.0, 5000.0, 5000.0, 1920.0, 1080.0), (1919.0, 1079.0));
        assert_eq!(land_delta(10.0, 10.0, -5000.0, -5000.0, 1920.0, 1080.0), (0.0, 0.0));
        // Exactly reaching the far edge is allowed.
        assert_eq!(land_delta(1918.0, 1078.0, 1.0, 1.0, 1920.0, 1080.0), (1919.0, 1079.0));
        // A pointer already lost outside comes back rather than staying out.
        assert_eq!(land_delta(5700.0, 5320.0, 0.0, 0.0, 1920.0, 1080.0), (1919.0, 1079.0));
        // A degenerate display must not produce bounds the wrong way round.
        assert_eq!(land_delta(5.0, 5.0, 0.0, 0.0, 0.0, 0.0), (0.0, 0.0));
    }

    /// An absolute move after a relative one is just an absolute move -- which is
    /// how the client ends the mode, so it has to keep working without any state
    /// being unwound here.
    #[test]
    fn an_absolute_move_after_a_relative_one_still_goes_where_it_says() {
        let mut inj = Injector::new();
        inj.decide_mouse(&mouse(RELATIVE, 100, 100));
        assert_eq!(
            inj.decide_mouse(&mouse(0, 640, 360)),
            MouseAction::MoveOrDrag { ty: 0, x: 640.0, y: 360.0 }
        );
    }

    // -- drag state ----------------------------------------------------------

    /// Moving with a button held is a drag, not a move: posting a plain move
    /// mid-drag breaks text selection and drag-and-drop.
    #[test]
    fn a_move_while_a_button_is_held_becomes_a_drag() {
        let mut inj = Injector::new();
        assert_eq!(
            inj.decide_mouse(&mouse(0, 10, 10)),
            MouseAction::MoveOrDrag { ty: 0, x: 10.0, y: 10.0 }
        );
        inj.decide_mouse(&mouse(LEFT | DOWN, 0, 0));
        assert_eq!(
            inj.decide_mouse(&mouse(0, 20, 20)),
            MouseAction::MoveOrDrag { ty: 3, x: 20.0, y: 20.0 },
            "a move with the left button down is a left-drag"
        );
        inj.decide_mouse(&mouse(RIGHT | DOWN, 0, 0));
        inj.decide_mouse(&mouse(LEFT | UP, 0, 0));
        assert_eq!(
            inj.decide_mouse(&mouse(0, 30, 30)),
            MouseAction::MoveOrDrag { ty: 4, x: 30.0, y: 30.0 },
            "with only the right button left down it is a right-drag"
        );
    }

    #[test]
    fn releasing_every_button_returns_to_plain_moves() {
        let mut inj = Injector::new();
        inj.decide_mouse(&mouse(LEFT | DOWN, 0, 0));
        inj.decide_mouse(&mouse(LEFT | UP, 0, 0));
        assert_eq!(
            inj.decide_mouse(&mouse(0, 40, 40)),
            MouseAction::MoveOrDrag { ty: 0, x: 40.0, y: 40.0 }
        );
    }

    // -- the bug that made typing do nothing ---------------------------------

    /// `chr` is a character, not a keycode. Typing "test" arrives as
    /// 116,101,115,116; posting those as Mac virtual keycodes types
    /// PageUp, F9, Home, PageUp -- which reads as keyboard input never arriving.
    #[test]
    fn chr_is_a_character_and_never_a_keycode() {
        let inj = Injector::new();
        for ch in "test".chars() {
            let ev = key(key_event::Union::chr(ch as u32), true, false, &[]);
            match inj.decide_key(&ev) {
                KeyAction::Char { cp, down, then_up, .. } => {
                    assert_eq!(cp, ch as u32);
                    assert!(down);
                    assert!(!then_up);
                }
                other => panic!("chr must be typed as a character, got {:?}", other),
            }
        }
    }

    /// The characters "test" actually sends, spelled out, so a regression is
    /// legible in the failure rather than hidden behind a conversion.
    #[test]
    fn the_word_test_arrives_as_ascii_codepoints() {
        let inj = Injector::new();
        let sent: Vec<u32> = "test"
            .chars()
            .map(|c| match inj.decide_key(&key(key_event::Union::chr(c as u32), true, false, &[])) {
                KeyAction::Char { cp, .. } => cp,
                other => panic!("expected a character, got {:?}", other),
            })
            .collect();
        assert_eq!(sent, vec![116, 101, 115, 116]);
    }

    // -- keys, modifiers, and press ------------------------------------------

    #[test]
    fn control_keys_resolve_to_mac_keycodes() {
        let inj = Injector::new();
        let code = |k: ControlKey| {
            match inj.decide_key(&key(
                key_event::Union::control_key(ProtobufEnumOrUnknown::new(k)),
                true,
                false,
                &[],
            )) {
                KeyAction::Keycode { code, .. } => code,
                other => panic!("expected a keycode, got {:?}", other),
            }
        };
        assert_eq!(code(ControlKey::Escape), 53);
        assert_eq!(code(ControlKey::Return), 36);
        assert_eq!(code(ControlKey::Space), 49);
        assert_eq!(code(ControlKey::LeftArrow), 123);
    }

        /// `chr` is a character in Legacy and a keycode in Map, and the mode field
    /// is the only thing that says which. Getting this backwards types
    /// gibberish -- the original bug posted characters as keycodes and turned
    /// "test" into PageUp, F9, Home, PageUp.
    #[test]
    fn chr_is_a_character_in_legacy_and_a_keycode_in_map() {
        let inj = Injector::new();
        let mut ev = KeyEvent::new();
        ev.down = true;
        ev.union = Some(key_event::Union::chr(116)); // 't', or keycode 116 = PageUp

        // Unset mode is Legacy, which is what an older client sends.
        assert_eq!(
            inj.decide_key(&ev),
            KeyAction::Char { cp: 116, down: true, flags: 0, then_up: false },
            "an absent mode must read exactly as before"
        );

        ev.mode = protobuf::ProtobufEnumOrUnknown::new(KeyboardMode::Legacy);
        assert_eq!(
            inj.decide_key(&ev),
            KeyAction::Char { cp: 116, down: true, flags: 0, then_up: false }
        );

        ev.mode = protobuf::ProtobufEnumOrUnknown::new(KeyboardMode::Map);
        assert_eq!(
            inj.decide_key(&ev),
            KeyAction::PlatformKeycode { code: 116, down: true, flags: 0, then_up: false },
            "in Map the client sends a keycode for the platform we reported, which is a Mac keycode only on the Mac"
        );
    }

    /// Translate mode also sends a translated keycode; it differs from Map in
    /// what the *client* does before sending, not in what arrives.
    #[test]
    fn translate_mode_is_read_as_a_keycode_too() {
        let inj = Injector::new();
        let mut ev = KeyEvent::new();
        ev.down = true;
        ev.union = Some(key_event::Union::chr(49));
        ev.mode = protobuf::ProtobufEnumOrUnknown::new(KeyboardMode::Translate);
        assert_eq!(
            inj.decide_key(&ev),
            KeyAction::PlatformKeycode { code: 49, down: true, flags: 0, then_up: false }
        );
    }

    /// A control key is platform-independent and means the same in every mode.
    #[test]
    fn control_keys_are_unaffected_by_the_mode() {
        let inj = Injector::new();
        let mut ev = KeyEvent::new();
        ev.down = true;
        ev.union = Some(key_event::Union::control_key(
            protobuf::ProtobufEnumOrUnknown::new(ControlKey::Return),
        ));
        let legacy = inj.decide_key(&ev);
        ev.mode = protobuf::ProtobufEnumOrUnknown::new(KeyboardMode::Map);
        assert_eq!(legacy, inj.decide_key(&ev));
    }

#[test]
    fn a_control_key_with_no_mac_equivalent_is_reported_unmapped() {
        let inj = Injector::new();
        let ev = key(
            key_event::Union::control_key(ProtobufEnumOrUnknown::new(ControlKey::Snapshot)),
            true,
            false,
            &[],
        );
        assert_eq!(inj.decide_key(&ev), KeyAction::Unmapped);
    }

    #[test]
    fn modifiers_become_cgevent_flags() {
        let inj = Injector::new();
        let flags = |mods: &[ControlKey]| {
            match inj.decide_key(&key(key_event::Union::chr('c' as u32), true, false, mods)) {
                KeyAction::Char { flags, .. } => flags,
                other => panic!("expected a character, got {:?}", other),
            }
        };
        assert_eq!(flags(&[]), 0);
        assert_eq!(flags(&[ControlKey::Shift]), 0x0002_0000);
        assert_eq!(flags(&[ControlKey::Control]), 0x0004_0000);
        assert_eq!(flags(&[ControlKey::Alt]), 0x0008_0000);
        assert_eq!(flags(&[ControlKey::Meta]), 0x0010_0000);
        // Cmd+Shift, the shape a shortcut arrives in.
        assert_eq!(flags(&[ControlKey::Meta, ControlKey::Shift]), 0x0012_0000);
    }

    /// Left and right modifiers carry the same flag; the distinction is not one
    /// CGEventFlags makes.
    #[test]
    fn right_hand_modifiers_match_their_left_hand_flags() {
        let inj = Injector::new();
        let flags = |m: ControlKey| {
            match inj.decide_key(&key(key_event::Union::chr('a' as u32), true, false, &[m])) {
                KeyAction::Char { flags, .. } => flags,
                other => panic!("expected a character, got {:?}", other),
            }
        };
        assert_eq!(flags(ControlKey::RShift), flags(ControlKey::Shift));
        assert_eq!(flags(ControlKey::RControl), flags(ControlKey::Control));
        assert_eq!(flags(ControlKey::RAlt), flags(ControlKey::Alt));
        assert_eq!(flags(ControlKey::RWin), flags(ControlKey::Meta));
    }

    /// `press` means the client wants a full down-and-up from one message.
    #[test]
    fn press_asks_for_a_release_as_well() {
        let inj = Injector::new();
        let ev = key(key_event::Union::chr('x' as u32), true, true, &[]);
        match inj.decide_key(&ev) {
            KeyAction::Char { then_up, .. } => assert!(then_up),
            other => panic!("expected a character, got {:?}", other),
        }
    }

    #[test]
    fn unicode_and_seq_pass_through_without_a_keycode() {
        let inj = Injector::new();
        match inj.decide_key(&key(key_event::Union::unicode(0x00e9), true, false, &[])) {
            KeyAction::Unicode { cp, .. } => assert_eq!(cp, 0x00e9),
            other => panic!("expected unicode, got {:?}", other),
        }
        match inj.decide_key(&key(key_event::Union::seq("hi".into()), true, false, &[])) {
            KeyAction::Seq(s) => assert_eq!(s, "hi"),
            other => panic!("expected a sequence, got {:?}", other),
        }
    }

    #[test]
    fn a_key_event_with_no_field_set_is_ignored() {
        let inj = Injector::new();
        let mut ev = KeyEvent::new();
        ev.down = true;
        assert_eq!(inj.decide_key(&ev), KeyAction::Ignore);
    }
}
