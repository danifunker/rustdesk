//! Screen capture for Solaris 10, over the C shim in `capture_shim.c`.
//!
//! The shape of this module follows the IRIX port's, because the C contract
//! underneath is the same one -- a persistent full-screen canvas, plus a poll
//! that reports what changed. What differs is what the server can do, and two
//! of those differences reach up into this API:
//!
//! * **Damage carries no pixels.** SGI-SCREEN-CAPTURE handed the IRIX port the
//!   changed rectangles *and* their contents in one round trip. The standard
//!   DAMAGE extension reports rectangles only, so a poll that sees damage still
//!   reads the screen -- 5.3 ms over MIT-SHM, measured on the Blade at
//!   1280x1024, against 200 ms for the core-protocol path. A poll that sees no
//!   damage reads nothing at all, which is what makes that acceptable.
//! * **The cursor is not in the image.** X does not composite it into a
//!   drawable's contents, so [`Capturer::cursor_embedded`] is always false here
//!   and the agent sends a shape. XFIXES has the real one.
//!
//! Byte order is **A,R,G,B** in memory -- the PowerPC Mac's order, not the IRIX
//! port's A,B,G,R. [`Capturer::pixel_order`] reports what the server actually
//! said rather than trusting that, because getting it wrong swaps red and blue
//! and that is miserable to spot over a remote display.

#![allow(dead_code)]

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uchar};

/// Horizontal strips the screen is divided into for change reporting. Same
/// value as the other two agents so `session.rs` needs no changes to run here.
pub const BANDS: usize = 64;

/// Most rectangles fetched from one poll. Beyond this the shim merges them
/// into their bounding box rather than dropping any.
const MAX_RECTS: usize = 64;

pub const PATH_DAMAGE: i32 = 0;
pub const PATH_SHM: i32 = 1;
pub const PATH_GETIMAGE: i32 = 2;

pub const ORDER_ARGB: i32 = 0;
pub const ORDER_BGRA: i32 = 1;

enum RdCapture {}

extern "C" {
    fn rd_display_size(w: *mut c_int, h: *mut c_int) -> c_int;
    fn rd_capture_open(display: *const c_char) -> *mut RdCapture;
    fn rd_capture_open_forced(display: *const c_char, max_path: c_int) -> *mut RdCapture;
    fn rd_capture_close(c: *mut RdCapture);
    fn rd_capture_width(c: *const RdCapture) -> c_int;
    fn rd_capture_height(c: *const RdCapture) -> c_int;
    fn rd_capture_depth(c: *const RdCapture) -> c_int;
    fn rd_capture_stride(c: *const RdCapture) -> c_int;
    fn rd_capture_path(c: *const RdCapture) -> c_int;
    fn rd_capture_pixel_order(c: *const RdCapture) -> c_int;
    fn rd_capture_cursor_embedded(c: *const RdCapture) -> c_int;
    fn rd_capture_buffer(c: *const RdCapture) -> *const c_uchar;
    fn rd_capture_full(c: *mut RdCapture) -> c_int;
    fn rd_capture_poll(c: *mut RdCapture, rects: *mut c_int, max_rects: c_int) -> c_int;
    fn rd_capture_invalidate(c: *mut RdCapture);
    fn rd_capture_read_rect(c: *mut RdCapture, x: c_int, y: c_int, w: c_int, h: c_int) -> c_int;
    fn rd_capture_last_error(c: *const RdCapture) -> *const c_char;
    fn rd_capture_is_dead(c: *const RdCapture) -> c_int;
}

/// A changed region of the screen, in screen coordinates.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

/// The screen's size over a bare X connection, with nothing allocated. The
/// message loop asks this repeatedly, so it must stay cheap.
pub fn display_size() -> Option<(i32, i32)> {
    let (mut w, mut h) = (0, 0);
    if unsafe { rd_display_size(&mut w, &mut h) } == 0 {
        Some((w, h))
    } else {
        None
    }
}

pub struct Capturer {
    inner: *mut RdCapture,
    width: usize,
    height: usize,
    stride: usize,
    rects: Vec<Rect>,
    scratch: Vec<c_int>,
}

impl Capturer {
    /// Open `$DISPLAY` with the best path the server allows.
    pub fn new() -> Result<Self, String> {
        Self::open(None, PATH_DAMAGE)
    }

    /// Open `display`, refusing any path better than `max_path`. Forcing
    /// [`PATH_GETIMAGE`] is the only honest way to test the fallback.
    pub fn open(display: Option<&str>, max_path: i32) -> Result<Self, String> {
        let c_display = display.map(|d| CString::new(d).unwrap_or_default());
        let ptr = unsafe {
            let p = c_display
                .as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(std::ptr::null());
            if max_path == PATH_DAMAGE {
                rd_capture_open(p)
            } else {
                rd_capture_open_forced(p, max_path)
            }
        };
        if ptr.is_null() {
            return Err(format!(
                "cannot capture {}",
                display.unwrap_or("the default display")
            ));
        }
        let width = unsafe { rd_capture_width(ptr) } as usize;
        let height = unsafe { rd_capture_height(ptr) } as usize;
        let stride = unsafe { rd_capture_stride(ptr) } as usize;
        Ok(Capturer {
            inner: ptr,
            width,
            height,
            stride,
            rects: Vec::with_capacity(MAX_RECTS),
            scratch: vec![0; MAX_RECTS * 4],
        })
    }

    pub fn width(&self) -> usize {
        self.width
    }

    pub fn height(&self) -> usize {
        self.height
    }

    pub fn stride(&self) -> usize {
        self.stride
    }

    pub fn bits_per_pixel(&self) -> usize {
        32
    }

    pub fn screen_depth(&self) -> usize {
        unsafe { rd_capture_depth(self.inner) as usize }
    }

    pub fn path(&self) -> i32 {
        unsafe { rd_capture_path(self.inner) }
    }

    pub fn path_name(&self) -> &'static str {
        match self.path() {
            PATH_DAMAGE => "DAMAGE + MIT-SHM",
            PATH_SHM => "MIT-SHM, full screen per poll",
            PATH_GETIMAGE => "XGetImage (slow fallback)",
            _ => "?",
        }
    }

    pub fn pixel_order(&self) -> i32 {
        unsafe { rd_capture_pixel_order(self.inner) }
    }

    pub fn pixel_order_name(&self) -> &'static str {
        match self.pixel_order() {
            ORDER_ARGB => "A,R,G,B",
            ORDER_BGRA => "B,G,R,A",
            _ => "?",
        }
    }

    /// Always false on X: the cursor is never in what we read.
    pub fn cursor_embedded(&self) -> bool {
        unsafe { rd_capture_cursor_embedded(self.inner) != 0 }
    }

    pub fn is_dead(&self) -> bool {
        unsafe { rd_capture_is_dead(self.inner) != 0 }
    }

    pub fn last_error(&self) -> String {
        unsafe {
            CStr::from_ptr(rd_capture_last_error(self.inner))
                .to_string_lossy()
                .into_owned()
        }
    }

    /// Rows per band. The last band is whatever is left over.
    pub fn band_rows(&self) -> usize {
        (self.height + BANDS - 1) / BANDS
    }

    /// The half-open row range of band `b`.
    pub fn band_range(&self, b: usize) -> (usize, usize) {
        let rows = self.band_rows();
        let y0 = (b * rows).min(self.height);
        let y1 = (y0 + rows).min(self.height);
        (y0, y1)
    }

    /// The canvas. Valid until the next capture; never mutated by the caller.
    pub fn buffer(&self) -> &[u8] {
        unsafe {
            let p = rd_capture_buffer(self.inner);
            if p.is_null() {
                &[]
            } else {
                std::slice::from_raw_parts(p, self.stride * self.height)
            }
        }
    }

    /// Read the whole screen, then hand back the canvas.
    pub fn frame(&mut self) -> &[u8] {
        unsafe {
            rd_capture_full(self.inner);
        }
        self.buffer()
    }

    /// Read whatever changed and report the rectangles. Empty means the screen
    /// is unchanged, and nothing was read.
    pub fn poll_rects(&mut self) -> &[Rect] {
        let n = unsafe {
            rd_capture_poll(
                self.inner,
                self.scratch.as_mut_ptr(),
                MAX_RECTS as c_int,
            )
        };
        self.rects.clear();
        if n > 0 {
            for i in 0..n as usize {
                self.rects.push(Rect {
                    x: self.scratch[i * 4],
                    y: self.scratch[i * 4 + 1],
                    w: self.scratch[i * 4 + 2],
                    h: self.scratch[i * 4 + 3],
                });
            }
        }
        &self.rects
    }

    /// What the last [`poll_rects`](Self::poll_rects) reported.
    pub fn last_rects(&self) -> &[Rect] {
        &self.rects
    }

    /// The band view of a poll, for the parts of the agent that think in
    /// strips. The rectangles are the finer answer where a caller wants it.
    pub fn dirty_bands(&mut self) -> [bool; BANDS] {
        let mut dirty = [false; BANDS];
        let rows = self.band_rows().max(1);
        for r in self.poll_rects() {
            let first = (r.y.max(0) as usize) / rows;
            let last = (((r.y + r.h - 1).max(0) as usize) / rows).min(BANDS - 1);
            for b in first..=last.min(BANDS - 1) {
                dirty[b] = true;
            }
        }
        dirty
    }

    /// Make the next poll re-read and report everything.
    pub fn invalidate(&mut self) {
        unsafe { rd_capture_invalidate(self.inner) }
    }

    /// Re-read one rectangle, for repairing a region the *peer* lost. Costs in
    /// proportion to the rectangle: this one goes through XGetImage.
    pub fn read_rect(&mut self, x: i32, y: i32, w: i32, h: i32) -> Result<(), String> {
        if unsafe { rd_capture_read_rect(self.inner, x, y, w, h) } == 0 {
            Ok(())
        } else {
            Err(self.last_error())
        }
    }
}

impl Drop for Capturer {
    fn drop(&mut self) {
        unsafe { rd_capture_close(self.inner) }
    }
}
