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

#[cfg(target_os = "macos")]
use std::os::raw::c_double;
use std::os::raw::{c_int, c_uint};

use crate::message_proto::{key_event, ControlKey, KeyEvent, MouseEvent};

// The shim reverse-maps characters to keycodes with UCKeyTranslate, which lives
// in Carbon; CoreGraphics alone cannot do it on this OS (see `rd_key_char`).
#[cfg(target_os = "macos")]
#[link(name = "Carbon", kind = "framework")]
extern "C" {}

#[cfg(target_os = "macos")]
extern "C" {
    fn rd_mouse(ty: c_int, x: c_double, y: c_double, button: c_int);
    fn rd_mouse_here(ty: c_int, button: c_int);
    fn rd_key_char(cp: c_uint, down: c_int, flags: c_uint);
    fn rd_keycode_for_char(cp: c_uint, needs_shift: *mut c_int) -> c_int;
    fn rd_release_modifiers();
    fn rd_scroll(dy: c_int, dx: c_int);
    fn rd_key(keycode: c_int, down: c_int);
    fn rd_key_unicode(cp: c_uint, down: c_int);
    fn rd_key_with_flags(keycode: c_int, down: c_int, flags: c_uint);
    fn rd_cursor_pos(x: *mut c_double, y: *mut c_double);
}

/// Let go of every modifier key.
///
/// Worth doing when a session starts: a modifier left held -- by a client that
/// disconnected mid-shortcut, or by an earlier build that stamped flags onto
/// events instead of pressing keys -- corrupts everything afterwards. A stuck
/// Control is the worst of them, because a Control-click is a right-click here,
/// so left-clicking silently starts opening context menus.
#[cfg(target_os = "macos")]
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
#[cfg(target_os = "macos")]
pub fn keycode_for_char(cp: u32) -> Option<(i32, bool)> {
    let mut shift: c_int = 0;
    let code = unsafe { rd_keycode_for_char(cp, &mut shift) };
    if code < 0 {
        None
    } else {
        Some((code, shift != 0))
    }
}

/// Where the system thinks the cursor is. Out-params, not a returned struct.
#[cfg(target_os = "macos")]
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
    /// A press or release. `at_cursor` means the event carried no coordinates,
    /// so the position must come from the system rather than from the message.
    Button { down: bool, button: i32, at_cursor: bool, x: f64, y: f64 },
    /// Wheel movement, already in the sense CoreGraphics wants.
    Scroll { dx: i32, dy: i32 },
    /// A `mask` whose low bits name no event we handle.
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
    /// A control key with no keycode on this platform.
    Unmapped,
    /// No key field set at all.
    Ignore,
}

pub struct Injector {
    /// Buttons currently held, so a move can be reported as the drag it is —
    /// posting a plain move mid-drag breaks selection and drag-and-drop.
    buttons_down: u8,
}

impl Injector {
    pub fn new() -> Self {
        Self { buttons_down: 0 }
    }

    /// Decide what a mouse event means, and track which buttons are held.
    ///
    /// `mask` packs two fields, matching `input_service.rs`: the low 3 bits are
    /// the kind (0 move, 1 down, 2 up, 3 wheel) and the rest is the button
    /// (1 left, 2 right, 4 middle).
    pub fn decide_mouse(&mut self, ev: &MouseEvent) -> MouseAction {
        let (x, y) = (ev.x as f64, ev.y as f64);
        let kind = ev.mask & 0x7;
        let button = ev.mask >> 3;
        let btn_idx = match button {
            2 => 1, // right
            4 => 2, // middle
            _ => 0, // left
        };
        // A press or release arrives with no coordinates at all — proto3 drops
        // zero-valued fields, so the whole message is `mask`. Clicking at the
        // (0, 0) that implies lands every click in the top-left corner; the
        // position has to come from the system, as it does upstream.
        let at_cursor = ev.x == 0 && ev.y == 0;
        match kind {
            0 => {
                let ty = if self.buttons_down & 1 != 0 {
                    3 // left-dragged
                } else if self.buttons_down & 2 != 0 {
                    4 // right-dragged
                } else {
                    0 // moved
                };
                MouseAction::MoveOrDrag { ty, x, y }
            }
            1 => {
                self.buttons_down |= button as u8;
                MouseAction::Button { down: true, button: btn_idx, at_cursor, x, y }
            }
            2 => {
                self.buttons_down &= !(button as u8);
                MouseAction::Button { down: false, button: btn_idx, at_cursor, x, y }
            }
            // Both axes, both negated -- `input_service.rs` does exactly this
            // for every platform except Windows. Using only `y` dropped
            // sideways swipes entirely, and not negating scrolled the wrong
            // way, which together is most of what "gestures" means on a
            // trackpad.
            3 => MouseAction::Scroll { dx: -ev.x, dy: -ev.y },
            _ => MouseAction::Ignore,
        }
    }

    /// Decide what a key event means. `press` asks for a full down-and-up.
    pub fn decide_key(&self, ev: &KeyEvent) -> KeyAction {
        let flags = modifier_flags(&ev.modifiers);
        let down = ev.down;
        let then_up = ev.press;
        match &ev.union {
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

    #[cfg(target_os = "macos")]
    pub fn mouse(&mut self, ev: &MouseEvent) {
        let action = self.decide_mouse(ev);
        log::debug!("mouse mask={:#x} -> {:?}", ev.mask, action);
        unsafe {
            match action {
                MouseAction::MoveOrDrag { ty, x, y } => rd_mouse(ty, x, y, 0),
                MouseAction::Button { down, button, at_cursor, x, y } => {
                    let ty = if down { 1 } else { 2 };
                    if at_cursor {
                        rd_mouse_here(ty, button);
                    } else {
                        rd_mouse(ty, x, y, button);
                    }
                }
                MouseAction::Scroll { dx, dy } => rd_scroll(dy, dx),
                MouseAction::Ignore => {}
            }
        }
    }

    #[cfg(target_os = "macos")]
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

    /// Wheel events carry both axes and are inverted relative to the wire,
    /// matching upstream's non-Windows path.
    #[test]
    fn a_wheel_event_carries_both_axes_negated() {
        let mut inj = Injector::new();
        assert_eq!(inj.decide_mouse(&mouse(WHEEL, 0, -3)), MouseAction::Scroll { dx: 0, dy: 3 });
        assert_eq!(
            inj.decide_mouse(&mouse(WHEEL, 5, 0)),
            MouseAction::Scroll { dx: -5, dy: 0 },
            "a sideways swipe must not be dropped"
        );
        assert_eq!(inj.decide_mouse(&mouse(WHEEL, -2, 4)), MouseAction::Scroll { dx: 2, dy: -4 });
    }

    #[test]
    fn an_unknown_kind_is_ignored_rather_than_guessed() {
        let mut inj = Injector::new();
        assert_eq!(inj.decide_mouse(&mouse(7, 1, 2)), MouseAction::Ignore);
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
