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

    fn rd_argb_to_i420_rect(
        src: *const c_uchar,
        src_len: usize,
        src_stride: c_int,
        yp: *mut c_uchar,
        up: *mut c_uchar,
        vp: *mut c_uchar,
        dst_w: c_int,
        dst_h: c_int,
        chroma_stride: c_int,
        factor: c_int,
        dx0: c_int,
        dy0: c_int,
        dx1: c_int,
        dy1: c_int,
    ) -> c_int;
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
    /// Public because `session.rs` reads them as fields on all three ports.
    pub width: usize,
    pub height: usize,
    stride: usize,
    rects: Vec<Rect>,
    scratch: Vec<c_int>,
    /// Bands the caller asked to be re-read regardless of what the server
    /// said. See [`invalidate_band`](Capturer::invalidate_band).
    forced: Vec<bool>,
    /// Set once `refresh` has seen the resolution move.
    stale: bool,
    /// When `refresh` last opened a connection, so it can rate-limit itself.
    last_refresh: Option<std::time::Instant>,
}

impl Capturer {
    /// Open `$DISPLAY` with the best path the server allows.
    ///
    /// `&'static str` rather than `String` to match the PowerPC and IRIX
    /// `Capturer`s, so `session.rs`'s `?` works against any of the three
    /// without a conversion. The detail goes to the log instead of to the
    /// caller, which is where it is readable anyway.
    pub fn new() -> Result<Self, &'static str> {
        Self::open(None, PATH_DAMAGE).map_err(|e| {
            log::error!("capture: {}", e);
            "capture: could not open the display"
        })
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
        if width == 0 || height == 0 || stride < width * 4 {
            let why = unsafe {
                CStr::from_ptr(rd_capture_last_error(ptr))
                    .to_string_lossy()
                    .into_owned()
            };
            unsafe { rd_capture_close(ptr) };
            return Err(format!(
                "implausible geometry {}x{} stride {}: {}",
                width, height, stride, why
            ));
        }
        Ok(Capturer {
            inner: ptr,
            width,
            height,
            stride,
            rects: Vec::with_capacity(MAX_RECTS),
            scratch: vec![0; MAX_RECTS * 4],
            forced: vec![false; BANDS],
            stale: false,
            last_refresh: None,
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

    /// `dirty_bands` with the Mac's sampling knobs spelled out.
    ///
    /// The knobs describe a sampled-checksum probe: how wide a run of bytes to
    /// read, and how far apart those runs are. Neither reaches anything here.
    /// On the damage path there is no probe read at all -- the server says what
    /// changed -- and on the hash fallback the sampling rate is fixed in
    /// `capture_shim.c` (`HASH_ROW_STEP`) rather than being a per-call dial.
    /// So the arguments are accepted and ignored rather than making the caller
    /// special-case the platform, and `--probe-display`'s sweep will report the
    /// same cost for every setting. That is the honest answer, not a broken
    /// one.
    pub fn dirty_bands_tuned(&mut self, _window: usize, _step: usize) -> [bool; BANDS] {
        self.dirty_bands()
    }

    /// Bytes one probe reads, for the same sweep.
    ///
    /// Zero on the damage path, and not because the probe is free: an idle poll
    /// touches no pixels, so there is no probe read to measure. On the
    /// fallbacks every poll reads the whole canvas before comparing it, so the
    /// honest number is the canvas.
    pub fn probe_bytes(&self, _window: usize, _step: usize) -> usize {
        if self.path() == PATH_DAMAGE {
            0
        } else {
            self.stride * self.height
        }
    }

    /// Make the next poll re-read and report everything.
    pub fn invalidate(&mut self) {
        unsafe { rd_capture_invalidate(self.inner) }
        self.forced.iter_mut().for_each(|f| *f = true);
    }

    /// Force one band to report dirty on the next poll.
    ///
    /// `session`'s rotating repair uses this. On the Mac it exists because a
    /// full-screen repair costs ~370 ms and has to be spread out; here the
    /// server tells us what changed, so a repair is only needed for pixels the
    /// peer may have lost rather than ones the agent failed to notice. Kept
    /// because the session logic is shared, and it costs nothing.
    pub fn invalidate_band(&mut self, b: usize) {
        if let Some(f) = self.forced.get_mut(b) {
            *f = true;
        }
    }

    /// Re-read the band's rows into the canvas, returning the rows it covers.
    ///
    /// The canvas is already current whenever the poll reported anything -- the
    /// shim reads the whole screen over MIT-SHM before it hands back
    /// rectangles -- so this only has to do real work when a band was
    /// force-invalidated, and then it reads just that band's rectangle.
    /// That read goes through `XGetImage` rather than shared memory, which is
    /// why it is worth confining to one band: the full-screen version of it
    /// costs 200 ms here.
    pub fn read_band(&mut self, b: usize) -> (usize, usize) {
        let (start, end) = self.band_range(b);
        if start >= end {
            return (start, end);
        }
        let forced = self.forced.get(b).copied().unwrap_or(false);
        if forced {
            unsafe {
                rd_capture_read_rect(
                    self.inner,
                    0,
                    start as c_int,
                    self.width as c_int,
                    (end - start) as c_int,
                );
            }
            if let Some(f) = self.forced.get_mut(b) {
                *f = false;
            }
        }
        (start, end)
    }

    /// Re-read the geometry, because the screen resolution may have changed.
    ///
    /// Returns true only when the geometry moved, meaning the encoder and the
    /// I420 buffers have to be rebuilt around the new size.
    ///
    /// A resolution change also invalidates the shared-memory canvas and the
    /// server's damage interest, both of which are sized at open time, so the
    /// honest answer is to report the change and let `session` rebuild the
    /// whole `Capturer`. Reporting it without acting on it would leave the
    /// canvas the wrong size, which is a crash rather than a wrong picture.
    pub fn refresh(&mut self) -> bool {
        // Rate-limited, because `session` calls this at the top of every pass
        // of the message loop and a resolution does not change several times a
        // second. It is a bare X connection rather than a whole capture
        // context, but a connection per frame is still not free.
        match self.last_refresh {
            Some(t) if t.elapsed() < std::time::Duration::from_secs(5) => return false,
            _ => self.last_refresh = Some(std::time::Instant::now()),
        }
        let (w, h) = match display_size() {
            Some(wh) => wh,
            None => return false,
        };
        if w <= 0 || h <= 0 {
            return false;
        }
        let changed = w as usize != self.width || h as usize != self.height;
        if changed {
            log::warn!(
                "capture: display changed from {}x{} to {}x{}; the shm canvas and the \
                 damage interest are both sized at open time, so this Capturer is stale",
                self.width, self.height, w, h
            );
            self.stale = true;
        }
        changed
    }

    /// True once `refresh` has seen the resolution move. The canvas cannot be
    /// resized in place, so the caller must build a new `Capturer`.
    pub fn is_stale(&self) -> bool {
        self.stale
    }

    /// Downscale **and** convert to I420 in one walk of the canvas, over a
    /// rectangle of the destination.
    ///
    /// This is the frame loop's hot path. It replaces a downscale pass over
    /// the canvas, the full-size intermediate that pass would write, and
    /// `convert::argb_to_i420_rows`' pass back over it -- and unlike that
    /// shared converter it reads this machine's byte order directly. The two
    /// happen to agree on A,R,G,B, so the *conversion* here is not the reason
    /// for a private inner loop; the fusion and the destination rectangle are.
    ///
    /// `factor` must be a power of two; anything else is refused rather than
    /// quietly rounded, because the box average folds into the BT.601 weights
    /// as a shift and a per-pixel divide is not affordable here.
    ///
    /// Bounds are in destination pixels and are snapped outward to even.
    /// Returns false if the arguments did not make sense, in which case
    /// nothing was written.
    pub fn to_i420_rect(
        &self,
        img: &mut crate::convert::I420,
        factor: i32,
        dx0: i32,
        dy0: i32,
        dx1: i32,
        dy1: i32,
    ) -> bool {
        let src = unsafe { rd_capture_buffer(self.inner) };
        if src.is_null() {
            return false;
        }
        // The C loop reads byte 1 as red and byte 3 as blue. If the server
        // ever hands back the other order, converting anyway would swap red
        // and blue in every frame -- a fault that looks like the client's.
        if self.pixel_order() != ORDER_ARGB {
            return false;
        }
        let cs = img.chroma_stride() as c_int;
        let (w, h) = (img.width as c_int, img.height as c_int);
        let rc = unsafe {
            rd_argb_to_i420_rect(
                src,
                self.stride * self.height,
                self.stride as c_int,
                img.y.as_mut_ptr(),
                img.u.as_mut_ptr(),
                img.v.as_mut_ptr(),
                w,
                h,
                cs,
                factor as c_int,
                dx0 as c_int,
                dy0 as c_int,
                dx1 as c_int,
                dy1 as c_int,
            )
        };
        rc == 0
    }

    /// The whole screen through [`to_i420_rect`](Capturer::to_i420_rect).
    pub fn to_i420(&self, img: &mut crate::convert::I420, factor: i32) -> bool {
        let (w, h) = (img.width as i32, img.height as i32);
        self.to_i420_rect(img, factor, 0, 0, w, h)
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
