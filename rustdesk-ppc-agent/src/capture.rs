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
//! 1920x1080, ~135 ms at 1024x768, ~85 ms at 800x600.

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

pub struct Capturer {
    display: CGDirectDisplayID,
    pub width: usize,
    pub height: usize,
    bytes_per_row: usize,
    base: *mut c_void,
    /// RAM shadow of the framebuffer; see the module note on VRAM read cost.
    buf: Vec<u8>,
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
        unsafe {
            std::ptr::copy_nonoverlapping(self.base as *const u8, self.buf.as_mut_ptr(), n);
        }
        &self.buf
    }

    pub fn stride(&self) -> usize {
        self.bytes_per_row
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
