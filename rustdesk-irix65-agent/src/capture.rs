//! Screen capture for IRIX, over the SGI X extensions.
//!
//! The shape of this module is set by what `capture_shim.c` found the server
//! can do, and it is a much better hand than the Mac port was dealt:
//!
//! * **The server tracks damage.** SGI-SCREEN-CAPTURE hands back the changed
//!   rectangles *and* their pixels in one round trip. Measured on an emulated
//!   R5000 Indy at 1280x1024: an idle poll costs about 3 ms, a busy one about
//!   4 ms. The PowerPC port had to sample every eighth row and hash it to guess
//!   at the same answer; here it is simply told.
//! * **Pixels arrive 32-bit whatever the screen is.** This Indy runs an 8-bit
//!   pseudocolour visual, and ReadDisplay still returns true colour, so the
//!   capture path never touches a palette. Only the `XGetImage` fallback does.
//! * **The cursor is composited for us.** `XRD_READ_POINTER` is honoured, so
//!   the agent sets `cursor_embedded` and never sends a cursor shape. That
//!   retires the PPC port's synthetic-arrow workaround.
//!
//! # Byte order
//!
//! **A,B,G,R in memory** — 0xff first, red last. Measured with
//! `probes/xcapture.c`, not read off a header. It is not the Mac's A,R,G,B and
//! it is not libyuv's "ARGB" (which means B,G,R,A in memory); getting this
//! wrong swaps red and blue, which is miserable to spot over a remote display.
//!
//! # Bands
//!
//! The API keeps the PowerPC agent's band vocabulary — [`BANDS`] horizontal
//! strips, [`Capturer::dirty_bands`] — so `session.rs` needs no changes to run
//! here. The difference is underneath: a band is dirty because the server said
//! a rectangle overlapping it changed, not because a sampled checksum moved.
//! That means no missed changes, where the sampled version could miss anything
//! confined to unsampled rows. [`Capturer::dirty_rects`] exposes the finer
//! answer for a caller that wants to encode sub-band regions later.

#![allow(dead_code)]

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};

/// Horizontal strips the screen is divided into for change reporting. Same
/// value as the PowerPC agent so the two `session.rs` paths agree.
pub const BANDS: usize = 64;

/// Most rectangles fetched from one poll. Beyond this the shim merges them into
/// their bounding box rather than dropping any, so the canvas and the report
/// can never disagree.
///
/// **Raised from 64 after the merge showed up in a live session.** A busy
/// `xterm` reported enough rectangles to overflow 64, and the bounding box that
/// replaced them covered 434 of the frame's 1280 macroblocks where the actual
/// change was nearer 180 -- so the merge was costing a third of the conversion
/// and a third of the encode, on precisely the frames that were already the
/// expensive ones. Four kilobytes of `int` is not a reason to throw away a
/// damage report the server has already worked out.
const MAX_RECTS: usize = 256;

pub const PATH_DAMAGE: i32 = 0;
pub const PATH_READDISPLAY: i32 = 1;
pub const PATH_GETIMAGE: i32 = 2;

#[repr(C)]
struct RdCapture {
    _private: [u8; 0],
}

extern "C" {
    fn rd_capture_open(display: *const c_char) -> *mut RdCapture;
    fn rd_display_size(w: *mut c_int, h: *mut c_int) -> c_int;
    fn rd_capture_display_size(c: *mut RdCapture, w: *mut c_int, h: *mut c_int) -> c_int;
    fn rd_capture_open_forced(display: *const c_char, max_path: c_int) -> *mut RdCapture;
    fn rd_capture_close(c: *mut RdCapture);
    fn rd_capture_width(c: *const RdCapture) -> c_int;
    fn rd_capture_height(c: *const RdCapture) -> c_int;
    fn rd_capture_depth(c: *const RdCapture) -> c_int;
    fn rd_capture_stride(c: *const RdCapture) -> c_int;
    fn rd_capture_path(c: *const RdCapture) -> c_int;
    fn rd_capture_cursor_embedded(c: *const RdCapture) -> c_int;
    fn rd_capture_buffer(c: *const RdCapture) -> *const u8;
    fn rd_capture_full(c: *mut RdCapture) -> c_int;
    fn rd_capture_poll(c: *mut RdCapture, rects: *mut c_int, max_rects: c_int) -> c_int;
    fn rd_capture_invalidate(c: *mut RdCapture);
    fn rd_capture_read_rect(c: *mut RdCapture, x: c_int, y: c_int, w: c_int, h: c_int) -> c_int;
    fn rd_capture_last_error(c: *const RdCapture) -> *const c_char;
    fn rd_capture_is_dead(c: *const RdCapture) -> c_int;
    fn rd_scale_abgr(
        src: *const u8,
        sw: c_int,
        sh: c_int,
        src_stride: c_int,
        dst: *mut u8,
        factor: c_int,
    ) -> c_int;
    fn rd_abgr_to_i420_rect(
        src: *const u8,
        src_len: usize,
        src_stride: c_int,
        yp: *mut u8,
        up: *mut u8,
        vp: *mut u8,
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

/// One changed region of the screen, in screen coordinates.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

/// The main display's size, without building a `Capturer`.
///
/// Used when a peer logs in: the size the agent saw at startup may be hours
/// stale by then, and `PeerInfo` is what sizes the peer's canvas.
pub fn display_size() -> Option<(i32, i32)> {
    let (mut w, mut h) = (0 as c_int, 0 as c_int);
    if unsafe { rd_display_size(&mut w, &mut h) } != 0 {
        return None;
    }
    if w == 0 || h == 0 { None } else { Some((w, h)) }
}

pub struct Capturer {
    inner: *mut RdCapture,
    pub width: usize,
    pub height: usize,
    stride: usize,
    path: i32,
    cursor_embedded: bool,
    rects: Vec<c_int>,
    last: Vec<Rect>,
    scaled: Vec<u8>,
    /// Bands the caller asked to be re-read regardless of what the server said.
    forced: Vec<bool>,
    stale: bool,
    last_refresh: Option<std::time::Instant>,
}

impl Capturer {
    /// Build a capturer on `$DISPLAY`, or say why not.
    ///
    /// `&'static str` rather than `String` to match the PowerPC `Capturer`, so
    /// `session.rs`'s `?` works against either without a conversion.
    pub fn new() -> Result<Self, &'static str> {
        Self::open(None, PATH_DAMAGE).map_err(|e| {
            log::error!("capture: {}", e);
            "capture: could not open the display"
        })
    }

    /// Build one on a named display, refusing any path better than `max_path`.
    ///
    /// The `max_path` knob exists so the fallbacks can be exercised on purpose.
    /// A fallback whose first run is on someone else's machine, on the day the
    /// extensions turn out to be missing, is not a fallback.
    pub fn open(display: Option<&str>, max_path: i32) -> Result<Self, String> {
        let cname = display.map(|d| CString::new(d).unwrap_or_default());
        let ptr = cname.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
        let inner = unsafe { rd_capture_open_forced(ptr, max_path) };
        if inner.is_null() {
            return Err(format!(
                "rd_capture_open({}) failed",
                display.unwrap_or("$DISPLAY")
            ));
        }
        let width = unsafe { rd_capture_width(inner) } as usize;
        let height = unsafe { rd_capture_height(inner) } as usize;
        let stride = unsafe { rd_capture_stride(inner) } as usize;
        if width == 0 || height == 0 || stride < width * 4 {
            let why = unsafe { last_error(inner) };
            unsafe { rd_capture_close(inner) };
            return Err(format!("implausible geometry {}x{} stride {}: {}", width, height, stride, why));
        }
        Ok(Capturer {
            inner,
            width,
            height,
            stride,
            path: unsafe { rd_capture_path(inner) },
            cursor_embedded: unsafe { rd_capture_cursor_embedded(inner) } != 0,
            rects: vec![0; MAX_RECTS * 4],
            last: Vec::with_capacity(MAX_RECTS),
            scaled: Vec::new(),
            forced: vec![false; BANDS],
            stale: false,
            last_refresh: None,
        })
    }

    /// Which capture path is in use: [`PATH_DAMAGE`], [`PATH_READDISPLAY`] or
    /// [`PATH_GETIMAGE`]. Worth logging once — the three differ by two orders
    /// of magnitude in cost, and "the agent is slow" is otherwise unexplained.
    pub fn path(&self) -> i32 {
        self.path
    }

    pub fn path_name(&self) -> &'static str {
        match self.path {
            PATH_DAMAGE => "damage (SGI-SCREEN-CAPTURE)",
            PATH_READDISPLAY => "readdisplay (whole screen per frame)",
            PATH_GETIMAGE => "getimage (plain Xlib fallback)",
            _ => "unknown",
        }
    }

    /// True when the server composites the hardware cursor into the image, in
    /// which case the agent must set `SwitchDisplay.cursor_embedded` and never
    /// send a cursor shape.
    pub fn cursor_embedded(&self) -> bool {
        self.cursor_embedded
    }

    pub fn stride(&self) -> usize {
        self.stride
    }

    pub fn bits_per_pixel(&self) -> usize {
        32
    }

    /// The screen's own depth, which is 8 on plenty of these machines even
    /// though capture always yields 32.
    pub fn screen_depth(&self) -> usize {
        unsafe { rd_capture_depth(self.inner) as usize }
    }

    /// True once the X connection has died. The server does wedge and get
    /// restarted; the agent should drop this `Capturer` and build a new one
    /// rather than keep polling a corpse.
    pub fn is_dead(&self) -> bool {
        unsafe { rd_capture_is_dead(self.inner) != 0 }
    }

    pub fn last_error(&self) -> String {
        unsafe { last_error(self.inner) }
    }

    /// Rows per band, always even.
    ///
    /// Even because 4:2:0 chroma is shared across each 2x2 block, so a band
    /// boundary on an odd row makes the converter snap outwards and touch a row
    /// the band never read.
    pub fn band_rows(&self) -> usize {
        let r = (self.height + BANDS - 1) / BANDS;
        r + (r & 1)
    }

    /// `(start_row, end_row)` for a band, clamped to the screen.
    pub fn band_range(&self, b: usize) -> (usize, usize) {
        let rows = self.band_rows();
        let start = (b * rows).min(self.height);
        let end = ((b + 1) * rows).min(self.height);
        (start, end)
    }

    /// Read the whole screen and return the canvas.
    ///
    /// No copy: the server writes into shared memory that this process already
    /// maps, so the canvas *is* the frame. The PowerPC port had to bulk-copy out
    /// of uncached VRAM here.
    pub fn frame(&mut self) -> &[u8] {
        let ok = unsafe { rd_capture_full(self.inner) } == 0;
        if ok {
            // The whole screen is now in the canvas, so nothing is owed. Leaving
            // the forced flags set made `read_band` re-read all 64 bands
            // immediately afterwards: the screen was captured twice, and the
            // second pass was 64 separate round trips issued back to back. That
            // doubled the capture cost of every new session and is a plausible
            // trigger for the server wedge, since bursts of ReadDisplay traffic
            // are exactly what provokes it.
            self.forced.iter_mut().for_each(|f| *f = false);
        }
        self.buffer()
    }

    /// The canvas as it currently stands, without reading anything.
    pub fn buffer(&self) -> &[u8] {
        let p = unsafe { rd_capture_buffer(self.inner) };
        if p.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(p, self.stride * self.height) }
        }
    }

    /// Update the canvas and report which bands changed.
    ///
    /// Unlike the sampled version on the Mac this cannot miss a change: the
    /// server reports every damaged rectangle, and the pixels arrive with it.
    pub fn dirty_bands(&mut self) -> [bool; BANDS] {
        let mut dirty = [false; BANDS];
        let rows = self.band_rows().max(1);
        for r in self.poll_rects() {
            let first = (r.y.max(0) as usize) / rows;
            let last_row = (r.y + r.h - 1).max(0) as usize;
            let last = (last_row / rows).min(BANDS - 1);
            for b in first.min(BANDS - 1)..=last {
                dirty[b] = true;
            }
        }
        dirty
    }

    /// `dirty_bands` with the Mac's sampling knobs spelled out.
    ///
    /// The knobs describe a sampled-checksum probe: how wide a run to read and
    /// how far apart the runs are. This path has no sampling to tune — the
    /// server reports what changed — so the arguments are accepted and ignored
    /// rather than the caller being made to special-case the platform.
    /// `--probe-display`'s sweep will therefore show the same cost for every
    /// setting, which is the honest answer.
    pub fn dirty_bands_tuned(&mut self, _window: usize, _step: usize) -> [bool; BANDS] {
        self.dirty_bands()
    }

    /// Bytes a probe reads, for the same sweep.
    ///
    /// Zero, and not because the probe is free: the pixels arrive with the
    /// damage report rather than in a separate read, so there is no probe read
    /// to measure. Reporting a made-up number would make the sweep look
    /// meaningful when it is not.
    pub fn probe_bytes(&self, _window: usize, _step: usize) -> usize {
        0
    }

    /// Update the canvas and return the changed rectangles themselves.
    ///
    /// The finer answer behind `dirty_bands`, kept because encoding only the
    /// changed rectangles is the obvious next saving once the band path works.
    pub fn poll_rects(&mut self) -> &[Rect] {
        let n = unsafe {
            rd_capture_poll(self.inner, self.rects.as_mut_ptr(), MAX_RECTS as c_int)
        };
        // A poll that reported the whole screen did a full read inside the shim,
        // so the bands are current for the same reason as in `frame`.
        if n == 1 && self.rects[2] == self.width as c_int && self.rects[3] == self.height as c_int {
            self.forced.iter_mut().for_each(|f| *f = false);
        }
        self.last.clear();
        if n > 0 {
            for i in 0..(n as usize).min(MAX_RECTS) {
                self.last.push(Rect {
                    x: self.rects[i * 4],
                    y: self.rects[i * 4 + 1],
                    w: self.rects[i * 4 + 2],
                    h: self.rects[i * 4 + 3],
                });
            }
        }
        &self.last
    }

    /// The rectangles from the most recent poll, without polling again.
    pub fn last_rects(&self) -> &[Rect] {
        &self.last
    }

    /// Present for interface parity with the PowerPC agent, where reading the
    /// dirty bands is a separate and expensive step. Here `dirty_bands` already
    /// brought the pixels with it, so this only hands the canvas back.
    pub fn read_bands(&mut self, _dirty: &[bool; BANDS]) -> &[u8] {
        self.buffer()
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
    /// The canvas is already current on the damage path — the server wrote into
    /// it during the poll — so this only has to do real work when a band was
    /// force-invalidated, and then it reads just that band's rectangle.
    pub fn read_band(&mut self, b: usize) -> (usize, usize) {
        let (start, end) = self.band_range(b);
        if start >= end {
            return (start, end);
        }
        let forced = self.forced.get(b).copied().unwrap_or(false);
        if forced {
            let mut r = [0i32; 4];
            r[0] = 0;
            r[1] = start as i32;
            r[2] = self.width as i32;
            r[3] = (end - start) as i32;
            unsafe {
                rd_capture_read_rect(self.inner, r[0], r[1], r[2], r[3]);
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
    /// On IRIX a resolution change also invalidates the shared-memory canvas and
    /// the server's damage interest, both of which are sized at open time, so
    /// the honest answer is to report the change and let `session` rebuild the
    /// whole `Capturer`. Reporting it without acting on it would leave the
    /// canvas the wrong size, which is a crash rather than a wrong picture.
    pub fn refresh(&mut self) -> bool {
        // Rate-limited, because `session` calls this at the top of every pass of
        // the message loop and a resolution does not change several times a
        // second. It is cheap now -- one round trip on the connection this
        // Capturer already holds -- but there is still no reason to ask often.
        //
        // It used to call `display_size`, which opens a fresh connection
        // because Xlib caches the geometry from connection setup. That leaked
        // about 16.5 MB of address space per call on this server and killed the
        // agent with "process or stack limit exceeded" after roughly ten
        // minutes of a live session. See rd_capture_display_size.
        match self.last_refresh {
            Some(t) if t.elapsed() < std::time::Duration::from_secs(5) => return false,
            _ => self.last_refresh = Some(std::time::Instant::now()),
        }
        let (mut w, mut h) = (0 as c_int, 0 as c_int);
        if unsafe { rd_capture_display_size(self.inner, &mut w, &mut h) } != 0 {
            return false;
        }
        let (w, h) = (w as i32, h as i32);
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

    /// Box-filter the canvas down by an integer factor, returning the scaled
    /// buffer and its width.
    ///
    /// Encode cost is linear in pixel count, so this is the agent's main lever:
    /// halving each dimension is a 4x saving. Wired to the peer's requested
    /// image quality, which can change mid-session.
    pub fn scaled(&mut self, factor: i32) -> (&[u8], usize, usize) {
        if factor <= 1 {
            let h = self.height;
            let w = self.width;
            return (self.buffer(), w, h);
        }
        let dw = self.width / factor as usize;
        let dh = self.height / factor as usize;
        self.scaled.resize(dw * dh * 4, 0);
        let src = unsafe { rd_capture_buffer(self.inner) };
        let got = unsafe {
            rd_scale_abgr(
                src,
                self.width as c_int,
                self.height as c_int,
                self.stride as c_int,
                self.scaled.as_mut_ptr(),
                factor,
            )
        };
        if got as usize != dw {
            return (&[], 0, 0);
        }
        (&self.scaled, dw, dh)
    }

    /// Downscale **and** convert to I420 in one walk of the canvas, over a
    /// rectangle of the destination.
    ///
    /// This is the frame loop's hot path and it replaces three separate costs:
    /// `scaled()`'s pass over the canvas, the full-size ABGR intermediate it
    /// wrote, and `convert::argb_to_i420_rows`' pass back over that. It also
    /// fixes the byte order — the shared converter reads A,R,G,B, which is the
    /// Mac's framebuffer and not this one.
    ///
    /// `factor` must be a power of two; anything else is refused rather than
    /// quietly rounded, because the box average folds into the BT.601 weights
    /// as a shift and a per-pixel divide is not affordable here.
    ///
    /// Bounds are in destination pixels and are snapped outward to even.
    /// Returns false if the arguments did not make sense, in which case nothing
    /// was written.
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
        let cs = img.chroma_stride() as c_int;
        let (w, h) = (img.width as c_int, img.height as c_int);
        let rc = unsafe {
            rd_abgr_to_i420_rect(
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

    /// The whole screen through `to_i420_rect`.
    pub fn to_i420(&self, img: &mut crate::convert::I420, factor: i32) -> bool {
        let (w, h) = (img.width as i32, img.height as i32);
        self.to_i420_rect(img, factor, 0, 0, w, h)
    }
}


unsafe fn last_error(c: *const RdCapture) -> String {
    let p = rd_capture_last_error(c);
    if p.is_null() {
        "unknown".to_owned()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

impl Drop for Capturer {
    fn drop(&mut self) {
        if !self.inner.is_null() {
            unsafe { rd_capture_close(self.inner) };
            self.inner = std::ptr::null_mut();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Band mapping is pure arithmetic, so it can be checked without a display.
    #[test]
    fn a_rect_marks_every_band_it_touches() {
        // 1024 rows over 64 bands is 16 rows each.
        let rows = 16usize;
        let band_of = |y: usize| (y / rows).min(BANDS - 1);
        assert_eq!(band_of(0), 0);
        assert_eq!(band_of(15), 0);
        assert_eq!(band_of(16), 1);
        assert_eq!(band_of(1023), 63);
        assert_eq!(band_of(99999), 63);
    }
}
