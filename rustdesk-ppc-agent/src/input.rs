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

use std::os::raw::{c_double, c_int, c_uint};

use crate::message_proto::{key_event, ControlKey, KeyEvent, MouseEvent};

// The shim reverse-maps characters to keycodes with UCKeyTranslate, which lives
// in Carbon; CoreGraphics alone cannot do it on this OS (see `rd_key_char`).
#[link(name = "Carbon", kind = "framework")]
extern "C" {}

extern "C" {
    fn rd_mouse(ty: c_int, x: c_double, y: c_double, button: c_int);
    fn rd_mouse_here(ty: c_int, button: c_int);
    fn rd_key_char(cp: c_uint, down: c_int, flags: c_uint);
    fn rd_scroll(dy: c_int);
    fn rd_key(keycode: c_int, down: c_int);
    fn rd_key_unicode(cp: c_uint, down: c_int);
    fn rd_key_with_flags(keycode: c_int, down: c_int, flags: c_uint);
    fn rd_cursor_pos(x: *mut c_double, y: *mut c_double);
}

/// Where the system thinks the cursor is. Out-params, not a returned struct.
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

pub struct Injector {
    /// Buttons currently held, so a move can be reported as the drag it is —
    /// posting a plain move mid-drag breaks selection and drag-and-drop.
    buttons_down: u8,
}

impl Injector {
    pub fn new() -> Self {
        Self { buttons_down: 0 }
    }

    pub fn mouse(&mut self, ev: &MouseEvent) {
        let (x, y) = (ev.x as f64, ev.y as f64);
        let kind = ev.mask & 0x7;
        let button = ev.mask >> 3;
        let btn_idx = match button {
            2 => 1, // right
            4 => 2, // middle
            _ => 0, // left
        };
        log::debug!("mouse mask={:#x} kind={} button={} x={} y={}", ev.mask, kind, button, ev.x, ev.y);
        // A press or release arrives with no coordinates at all — proto3 drops
        // zero-valued fields, so the whole message is `mask`. Clicking at the
        // (0, 0) that implies lands every click in the top-left corner; the
        // position has to come from the system, as it does upstream.
        let positionless = ev.x == 0 && ev.y == 0;
        unsafe {
            match kind {
                0 => {
                    let ty = if self.buttons_down & 1 != 0 {
                        3 // left-dragged
                    } else if self.buttons_down & 2 != 0 {
                        4 // right-dragged
                    } else {
                        0 // moved
                    };
                    rd_mouse(ty, x, y, 0);
                }
                1 => {
                    self.buttons_down |= button as u8;
                    if positionless {
                        rd_mouse_here(1, btn_idx);
                    } else {
                        rd_mouse(1, x, y, btn_idx);
                    }
                }
                2 => {
                    self.buttons_down &= !(button as u8);
                    if positionless {
                        rd_mouse_here(2, btn_idx);
                    } else {
                        rd_mouse(2, x, y, btn_idx);
                    }
                }
                3 => rd_scroll(ev.y),
                _ => {}
            }
        }
    }

    pub fn key(&mut self, ev: &KeyEvent) {
        let flags = modifier_flags(&ev.modifiers);
        let down = if ev.down { 1 } else { 0 };
        log::debug!(
            "key down={} press={} flags={:#x} union={}",
            ev.down,
            ev.press,
            flags,
            match &ev.union {
                Some(key_event::Union::control_key(c)) => format!("control_key({:?})", c),
                Some(key_event::Union::chr(c)) => format!("chr({})", c),
                Some(key_event::Union::unicode(u)) => format!("unicode({})", u),
                Some(key_event::Union::seq(s)) => format!("seq({:?})", s),
                None => "none".to_string(),
            }
        );
        unsafe {
            match &ev.union {
                Some(key_event::Union::control_key(ck)) => {
                    match control_key_to_keycode(ck.enum_value_or_default()) {
                        Some(code) => {
                            if flags != 0 {
                                rd_key_with_flags(code, down, flags);
                            } else {
                                rd_key(code, down);
                            }
                            // `press` means a full down+up in one message.
                            if ev.press {
                                rd_key(code, 0);
                            }
                        }
                        None => log::debug!("unmapped control key {:?}", ck),
                    }
                }
                // A character, *not* a keycode: typing "test" arrives as
                // chr(116) chr(101) chr(115) chr(116). Treating those as Mac
                // virtual keycodes typed PageUp/F9/Home/PageUp, which looked
                // exactly like keyboard input not arriving at all. The shim maps
                // the character back to a keycode through the current layout so
                // that modifiers still compose into shortcuts.
                Some(key_event::Union::chr(c)) => {
                    rd_key_char(*c, down, flags);
                    if ev.press {
                        rd_key_char(*c, 0, flags);
                    }
                }
                // A character, with no keycode implied. Typed via the event's
                // unicode string, which avoids needing a layout-specific map.
                Some(key_event::Union::unicode(u)) => {
                    rd_key_unicode(*u, down);
                    if ev.press {
                        rd_key_unicode(*u, 0);
                    }
                }
                Some(key_event::Union::seq(s)) => {
                    for ch in s.chars() {
                        rd_key_unicode(ch as c_uint, 1);
                        rd_key_unicode(ch as c_uint, 0);
                    }
                }
                None => {}
            }
        }
    }
}
