//! Screen capture for Mac OS X 10.4/10.5 via direct framebuffer access.
//!
//! Upstream's `libs/scrap/src/quartz/` uses `CGDisplayStream`, which is 10.8+ and
//! simply absent here. The pre-10.6 route is `CGDisplayBaseAddress`: deprecated
//! in 10.6 and gone in 10.7, but this target never gets there.
//!
//! Measured on the G5 (Leopard 10.5.8, 1920x1080):
//!
//! ```text
//!   bpr = 7680 (== width*4, no row padding)   bpp = 32   bps = 8   spp = 3
//!   base(no capture) = 0xb0028000
//!   first pixels: ff db dd df
//! ```
//!
//! Two things that matter and were verified rather than assumed:
//!
//! * **No `CGDisplayCapture` is required.** Capturing the display would give
//!   exclusive access and blank the screen for whoever is sitting at the machine
//!   — useless for remote *assistance*. Reading the framebuffer directly works
//!   without it.
//! * **Byte order is A,R,G,B in memory** — the natural big-endian ARGB32. Note
//!   libyuv's `ARGB` means the little-endian layout, i.e. B,G,R,A in memory, so
//!   its `ARGBToI420` is *not* the right entry point here. See `crate::convert`.
//!
//! # Never read the framebuffer per-pixel
//!
//! The framebuffer is uncached VRAM, so every scattered read is a bus
//! transaction. Measured on the G5 at 1920x1080 (7.9 MB):
//!
//! ```text
//!   VRAM -> RAM memcpy :  347 ms  (22.8 MB/s)
//!   ARGB->Y from RAM   :   14 ms
//!   ARGB->Y from VRAM  : 3070 ms   <-- 220x worse
//! ```
//!
//! So [`Capturer::frame`] bulk-copies into a RAM buffer and hands that out. The
//! copy dominates, which makes capture cost linear in *resolution*: ~350 ms at
//! 1920x1080, ~126 ms at 1024x768, ~77 ms at 800x600.
//!
//! # Reading less is the only real optimisation
//!
//! Read cost is purely proportional to bytes, with no penalty for striding —
//! measured at 1920x1080:
//!
//! ```text
//!   full frame     348.6 ms      every  8th row   41.5 ms
//!   every 2nd row  166.1 ms      every 16th row   20.9 ms
//!   every 4th row   82.9 ms      every 32nd row   10.4 ms
//! ```
//!
//! A desktop is mostly static, so paying 348 ms to discover nothing changed is
//! the real waste. [`Capturer::dirty_bands`] samples every 16th row (~21 ms) and
//! returns which horizontal bands moved; [`Capturer::read_bands`] then reads only
//! those. No codec choice affects any of this — the cost is a raw memory read.

use std::os::raw::{c_int, c_void};

pub type CGDirectDisplayID = u32;

#[link(name = "ApplicationServices", kind = "framework")]
extern "C" {
    fn CGMainDisplayID() -> CGDirectDisplayID;
    fn CGDisplayPixelsWide(d: CGDirectDisplayID) -> usize;
    fn CGDisplayPixelsHigh(d: CGDirectDisplayID) -> usize;
    fn CGDisplayBytesPerRow(d: CGDirectDisplayID) -> usize;
    fn CGDisplayBitsPerPixel(d: CGDirectDisplayID) -> usize;
    fn CGDisplayBaseAddress(d: CGDirectDisplayID) -> *mut c_void;
    fn CGDisplayHideCursor(d: CGDirectDisplayID) -> c_int;
    fn CGDisplayShowCursor(d: CGDirectDisplayID) -> c_int;
}

/// Re-read the framebuffer address every time rather than caching it.
///
/// The WindowServer may page-flip between buffers, in which case a pointer
/// captured once goes stale and every later read returns the same frozen image.
/// The call is cheap next to the copy that follows it.
#[inline]
fn current_base(d: CGDirectDisplayID, fallback: *mut c_void) -> *const u8 {
    let b = unsafe { CGDisplayBaseAddress(d) };
    if b.is_null() { fallback as *const u8 } else { b as *const u8 }
}

/// Horizontal bands the screen is divided into for change detection. 16 keeps
/// the probe cheap while still isolating a typical window or menu to a couple of
/// bands.
pub const BANDS: usize = 16;

/// Rows sampled per band when probing. Every 16th row over the whole screen
/// costs ~21 ms at 1920x1080 versus ~348 ms for the full frame.
const PROBE_ROW_STEP: usize = 16;

pub struct Capturer {
    display: CGDirectDisplayID,
    pub width: usize,
    pub height: usize,
    bytes_per_row: usize,
    base: *mut c_void,
    /// RAM shadow of the framebuffer; see the module note on VRAM read cost.
    buf: Vec<u8>,
    /// Per-band checksums from the last probe, for change detection.
    sums: Vec<u64>,
}

impl Capturer {
    pub fn new() -> Result<Self, &'static str> {
        unsafe {
            let display = CGMainDisplayID();
            let bpp = CGDisplayBitsPerPixel(display);
            if bpp != 32 {
                // 16-bit modes exist on this vintage; the converter assumes 32.
                return Err("display is not 32 bits per pixel");
            }
            let base = CGDisplayBaseAddress(display);
            if base.is_null() {
                return Err("CGDisplayBaseAddress returned NULL (no window server session?)");
            }
            let height = CGDisplayPixelsHigh(display);
            let bytes_per_row = CGDisplayBytesPerRow(display);
            Ok(Self {
                display,
                width: CGDisplayPixelsWide(display),
                height,
                bytes_per_row,
                base,
                buf: vec![0; bytes_per_row * height],
                sums: vec![0; BANDS],
            })
        }
    }

    /// Snapshot the framebuffer into RAM and return the copy.
    ///
    /// One bulk `memcpy` out of VRAM — never read the returned slice's source
    /// directly, see the module note. The window server writes concurrently, so
    /// a frame may be torn; that is accepted, since the alternative is capturing
    /// the display and blanking it for whoever is sitting there.
    pub fn frame(&mut self) -> &[u8] {
        let n = self.bytes_per_row * self.height;
        if self.buf.len() != n {
            self.buf.resize(n, 0);
        }
        let base = current_base(self.display, self.base);
        unsafe {
            std::ptr::copy_nonoverlapping(base, self.buf.as_mut_ptr(), n);
        }
        &self.buf
    }

    pub fn stride(&self) -> usize {
        self.bytes_per_row
    }

    pub fn band_rows(&self) -> usize {
        (self.height + BANDS - 1) / BANDS
    }

    /// Sample every `PROBE_ROW_STEP`th row and report which bands changed since
    /// the previous probe. ~21 ms at 1920x1080 against ~348 ms for a full read.
    ///
    /// This is a sampled checksum, not a proof: a change confined entirely to
    /// unsampled rows is missed until something else in the band moves. That is
    /// the right trade for a screen-sharing agent, where the alternative is
    /// reading everything every time.
    pub fn dirty_bands(&mut self) -> [bool; BANDS] {
        let mut dirty = [false; BANDS];
        let rows_per = self.band_rows();
        let base = current_base(self.display, self.base);
        for b in 0..BANDS {
            let start = b * rows_per;
            let end = ((b + 1) * rows_per).min(self.height);
            let mut sum: u64 = 0;
            let mut row = start;
            while row < end {
                let off = row * self.bytes_per_row;
                let line = unsafe {
                    std::slice::from_raw_parts(base.add(off), self.bytes_per_row)
                };
                // Sample within the row too; the whole point is to touch few bytes.
                let mut c = 0;
                while c < line.len() {
                    sum = sum.wrapping_mul(31).wrapping_add(line[c] as u64);
                    c += 64;
                }
                row += PROBE_ROW_STEP;
            }
            if sum != self.sums[b] {
                dirty[b] = true;
                self.sums[b] = sum;
            }
        }
        dirty
    }

    /// Copy only the given bands out of VRAM into the shadow. Cost is
    /// proportional to the number of bands read.
    pub fn read_bands(&mut self, dirty: &[bool; BANDS]) -> &[u8] {
        let n = self.bytes_per_row * self.height;
        if self.buf.len() != n {
            self.buf.resize(n, 0);
        }
        let rows_per = self.band_rows();
        let base = current_base(self.display, self.base);
        for b in 0..BANDS {
            if !dirty[b] {
                continue;
            }
            let start = b * rows_per;
            let end = ((b + 1) * rows_per).min(self.height);
            if start >= end {
                continue;
            }
            let off = start * self.bytes_per_row;
            let len = (end - start) * self.bytes_per_row;
            unsafe {
                std::ptr::copy_nonoverlapping(
                    base.add(off),
                    self.buf.as_mut_ptr().add(off),
                    len,
                );
            }
        }
        &self.buf
    }

    /// Force the next `dirty_bands` to report everything, e.g. when a new peer
    /// connects and needs a full frame regardless of what moved.
    pub fn invalidate(&mut self) {
        for s in self.sums.iter_mut() {
            *s = u64::MAX;
        }
    }

    /// Re-read geometry; the user may have changed resolution under us.
    /// Returns true if anything moved, in which case the encoder needs restarting.
    pub fn refresh(&mut self) -> bool {
        unsafe {
            let (w, h, bpr) = (
                CGDisplayPixelsWide(self.display),
                CGDisplayPixelsHigh(self.display),
                CGDisplayBytesPerRow(self.display),
            );
            let base = CGDisplayBaseAddress(self.display);
            let changed = w != self.width || h != self.height || bpr != self.bytes_per_row || base != self.base;
            self.width = w;
            self.height = h;
            self.bytes_per_row = bpr;
            if !base.is_null() {
                self.base = base;
            }
            changed
        }
    }

    pub fn hide_cursor(&self, hide: bool) {
        unsafe {
            if hide {
                CGDisplayHideCursor(self.display);
            } else {
                CGDisplayShowCursor(self.display);
            }
        }
    }
}
