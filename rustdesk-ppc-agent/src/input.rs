//! Mouse and keyboard injection via Quartz Event Services.
//!
//! Unlike capture, this needed no archaeology: `CGEventCreateMouseEvent`,
//! `CGEventCreateKeyboardEvent`, `CGEventCreateScrollWheelEvent` and `CGEventPost`
//! all date from 10.4 and are present on the G5 (verified against the 10.5 SDK
//! headers). Upstream's `libs/enigo` uses the same calls.
//!
//! `MouseEvent.mask` packs two fields, as in `libs/enigo` and RustDesk's client:
//!   * low 3 bits  — event type: 0 move, 1 button down, 2 button up, 3 wheel
//!   * bits 3..    — which button: 1 left, 2 right, 4 middle

use std::os::raw::{c_double, c_int, c_void};

use crate::message_proto::{KeyEvent, MouseEvent};

type CGEventRef = *mut c_void;
type CGEventSourceRef = *mut c_void;
type CGEventType = u32;
type CGKeyCode = u16;
type CGMouseButton = u32;
type CGEventTapLocation = u32;

const K_CG_HID_EVENT_TAP: CGEventTapLocation = 0;
const K_CG_EVENT_SOURCE_STATE_HID: c_int = 1;

const NX_MOUSEMOVED: CGEventType = 5;
const NX_LMOUSEDOWN: CGEventType = 1;
const NX_LMOUSEUP: CGEventType = 2;
const NX_RMOUSEDOWN: CGEventType = 3;
const NX_RMOUSEUP: CGEventType = 4;
const NX_OMOUSEDOWN: CGEventType = 25;
const NX_OMOUSEUP: CGEventType = 26;
const NX_LMOUSEDRAGGED: CGEventType = 6;
const NX_RMOUSEDRAGGED: CGEventType = 7;

const BTN_LEFT: CGMouseButton = 0;
const BTN_RIGHT: CGMouseButton = 1;
const BTN_CENTER: CGMouseButton = 2;

#[repr(C)]
#[derive(Clone, Copy)]
struct CGPoint {
    x: c_double,
    y: c_double,
}

#[link(name = "ApplicationServices", kind = "framework")]
extern "C" {
    fn CGEventSourceCreate(state: c_int) -> CGEventSourceRef;
    fn CGEventCreateMouseEvent(
        source: CGEventSourceRef,
        mouse_type: CGEventType,
        position: CGPoint,
        button: CGMouseButton,
    ) -> CGEventRef;
    fn CGEventCreateKeyboardEvent(
        source: CGEventSourceRef,
        keycode: CGKeyCode,
        keydown: bool,
    ) -> CGEventRef;
    fn CGEventCreateScrollWheelEvent(
        source: CGEventSourceRef,
        units: u32,
        wheel_count: u32,
        wheel1: i32,
    ) -> CGEventRef;
    fn CGEventPost(tap: CGEventTapLocation, event: CGEventRef);
    fn CFRelease(cf: *mut c_void);
}

pub struct Injector {
    source: CGEventSourceRef,
    /// Buttons currently held, so a move can be reported as the drag it is —
    /// posting a plain move while a button is down breaks selection and drag.
    buttons_down: u8,
    last: (f64, f64),
}

// The event source is only touched from the single session thread.
unsafe impl Send for Injector {}

impl Injector {
    pub fn new() -> Self {
        Self {
            source: unsafe { CGEventSourceCreate(K_CG_EVENT_SOURCE_STATE_HID) },
            buttons_down: 0,
            last: (0.0, 0.0),
        }
    }

    fn post_mouse(&self, ty: CGEventType, pt: CGPoint, button: CGMouseButton) {
        unsafe {
            let e = CGEventCreateMouseEvent(self.source, ty, pt, button);
            if !e.is_null() {
                CGEventPost(K_CG_HID_EVENT_TAP, e);
                CFRelease(e);
            }
        }
    }

    pub fn mouse(&mut self, ev: &MouseEvent) {
        let pt = CGPoint { x: ev.x as f64, y: ev.y as f64 };
        self.last = (pt.x, pt.y);
        let kind = ev.mask & 0x7;
        let button = ev.mask >> 3;
        match kind {
            0 => {
                // Report a move as a drag if a button is held.
                let ty = if self.buttons_down & 1 != 0 {
                    NX_LMOUSEDRAGGED
                } else if self.buttons_down & 2 != 0 {
                    NX_RMOUSEDRAGGED
                } else {
                    NX_MOUSEMOVED
                };
                self.post_mouse(ty, pt, BTN_LEFT);
            }
            1 => {
                let (ty, b) = match button {
                    2 => (NX_RMOUSEDOWN, BTN_RIGHT),
                    4 => (NX_OMOUSEDOWN, BTN_CENTER),
                    _ => (NX_LMOUSEDOWN, BTN_LEFT),
                };
                self.buttons_down |= button as u8;
                self.post_mouse(ty, pt, b);
            }
            2 => {
                let (ty, b) = match button {
                    2 => (NX_RMOUSEUP, BTN_RIGHT),
                    4 => (NX_OMOUSEUP, BTN_CENTER),
                    _ => (NX_LMOUSEUP, BTN_LEFT),
                };
                self.buttons_down &= !(button as u8);
                self.post_mouse(ty, pt, b);
            }
            3 => unsafe {
                // Wheel delta arrives in y; 1 = "line" units.
                let e = CGEventCreateScrollWheelEvent(self.source, 1, 1, ev.y);
                if !e.is_null() {
                    CGEventPost(K_CG_HID_EVENT_TAP, e);
                    CFRelease(e);
                }
            },
            _ => {}
        }
    }

    pub fn key(&mut self, ev: &KeyEvent) {
        // Only raw keycodes for now; ControlKey/unicode mapping comes with the
        // keymap table, which is a table-building exercise rather than a
        // platform one.
        if let Some(crate::message_proto::key_event::Union::chr(c)) = ev.union {
            unsafe {
                let e = CGEventCreateKeyboardEvent(self.source, c as CGKeyCode, ev.down);
                if !e.is_null() {
                    CGEventPost(K_CG_HID_EVENT_TAP, e);
                    CFRelease(e);
                }
            }
        }
    }
}

impl Drop for Injector {
    fn drop(&mut self) {
        if !self.source.is_null() {
            unsafe { CFRelease(self.source) };
        }
    }
}
