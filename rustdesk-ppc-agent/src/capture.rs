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
//! * **No `CGDisplayCapture` is required, and it must not be used.** Capturing
//!   the display takes exclusive control of it and registers the caller as an
//!   application (`CGSRegisterProcessAsApp`), which steals focus and dismisses
//!   any menu the person at the machine has open. Run a few times a second it
//!   makes the console unusable. It also aborts outright in a process with no
//!   GUI session. Reading the framebuffer directly needs none of it.
//! * **Byte order is A,R,G,B in memory** — the natural big-endian ARGB32. Note
//!   libyuv's `ARGB` means the little-endian layout, i.e. B,G,R,A in memory, so
//!   its `ARGBToI420` is *not* the right entry point here. See `crate::convert`.
//!
//! # The mapping is live; a "frozen" screen is a bug in the probe
//!
//! It is worth being explicit, because this was got wrong at length. The pointer
//! from `CGDisplayBaseAddress` tracks the screen in a long-lived process.
//! Measured over 140 s with nothing forced, sampling the menu-bar clock: six
//! distinct images, and every in-process read agreed with a freshly exec'd
//! process, which is known to see current pixels.
//!
//! What looked like a frozen framebuffer was [`hash_row`] sampling only alpha
//! bytes, so *every* screen compared equal to every other. A capture/release
//! cycle was added to "republish" the mapping and appeared to work, because
//! `killall Dock` was being used to force a change and launchd throttles
//! respawns to ten seconds -- the repaint landed next to the call being
//! credited for it. If capture ever looks stale again, suspect the comparison
//! before reaching for CoreGraphics.
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
//! the real waste. [`Capturer::dirty_bands`] samples every 8th row and returns
//! which horizontal bands moved; [`Capturer::read_bands`] then reads only those.
//! No codec choice affects any of this — the cost is a raw memory read.
//!
//! The probe reads *runs* and hashes them from RAM, rather than reading the
//! framebuffer a byte at a time — the same 12x mistake as reading per pixel,
//! one level down, and it survived here longer. See [`PROBE_WINDOW`].

#[cfg(target_os = "macos")]
use std::os::raw::{c_int, c_void};

pub type CGDirectDisplayID = u32;

#[cfg(target_os = "macos")]
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

#[cfg(target_os = "macos")]
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

/// Rows sampled per band when probing.
///
/// Every 8th row, not every 16th. A line of terminal text is about 12 pixels
/// tall, so at a 16-row stride a redrawn line can sit entirely between sampled
/// rows: the band is then declared clean and the old contents stay on the
/// peer's screen. Running `vi` over a previous `ls -l` showed exactly that --
/// some lines repainted, others still showing the old output. Halving the
/// stride roughly doubles the probe, ~15 ms to ~30 ms, which is worth it.
/// `session` also repairs anything still missed once the screen goes quiet.
const PROBE_ROW_STEP: usize = 8;

/// Bytes copied in one go at each sample point along a row, and the distance
/// from one sample point to the next.
///
/// **Copy in bulk, then hash from RAM.** The probe is bound by bytes, not by
/// how many reads it issues -- measured on the G5, every window size from 3 to
/// 1024 bytes came out at the same 1.9 MB/s, with time tracking volume exactly.
/// What made it 1.9 MB/s rather than the 22.8 MB/s a bulk copy gets was hashing
/// *straight out of the framebuffer*: a byte-at-a-time loop over uncached VRAM
/// is a bus transaction per byte. Staging the sampled runs into RAM with a
/// `memcpy` first and hashing those costs a twelfth as much for the same bytes,
/// which is the whole reason this reads runs rather than single pixels.
///
/// Given that, the window and the step are simply a coverage dial: a quarter of
/// every sampled row, against a sixteenth for the probe this replaced, for less
/// than half its cost. Coverage is what decides whether a single character
/// being typed is noticed now or at the next settle repaint.
const PROBE_WINDOW: usize = 128;
const PROBE_WINDOW_STEP: usize = 512;

/// FNV-1a, 32 bits. A 32-bit multiply rather than the 64-bit one a `u64`
/// checksum needs, which is not free on a 32-bit PowerPC when the loop runs
/// over half a megabyte per probe.
const FNV_PRIME: u32 = 16777619;

/// Pack `window` bytes out of every `step` of `line` into `dst`, returning how
/// many bytes landed there.
///
/// This is the only thing in the probe that touches the framebuffer, and it
/// touches it exclusively through `copy_from_slice`, which is a `memcpy`. That
/// is the entire point -- see [`PROBE_WINDOW`] for what happens when the bytes
/// are read individually instead.
///
/// Sampling runs rather than single pixels also makes the worst bug in this
/// file's history structurally impossible. Memory order is A,R,G,B and alpha is
/// 0xff across a desktop, so the earlier probe -- which sampled single bytes at
/// a stride that was a multiple of the 4-byte pixel -- read nothing but alpha,
/// came out constant, and made every screen compare equal to every other. It
/// presents exactly like a frozen framebuffer and cost a full debugging
/// session. A contiguous run cannot land only on alpha.
fn sample_row(line: &[u8], dst: &mut [u8], window: usize, step: usize) -> usize {
    if window == 0 || step == 0 {
        return 0; // a caller-supplied pattern; do not spin on a nonsense one
    }
    let mut p = 0;
    let mut w = 0;
    while w < line.len() {
        let n = window.min(line.len() - w).min(dst.len() - p);
        if n == 0 {
            break;
        }
        dst[p..p + n].copy_from_slice(&line[w..w + n]);
        p += n;
        w += step;
    }
    p
}

/// FNV-1a over a staged buffer. Called on RAM, never on the framebuffer.
fn hash_bytes(buf: &[u8], seed: u32) -> u32 {
    let mut sum = seed;
    // Iterate rather than index: mrustc emits C at -O1, where a bounds check
    // per byte over a quarter of a megabyte is not noise.
    for &b in buf {
        sum = (sum ^ b as u32).wrapping_mul(FNV_PRIME);
    }
    sum
}

#[cfg(test)]
mod tests {
    use super::{hash_bytes, sample_row, PROBE_WINDOW, PROBE_WINDOW_STEP};

    /// Sample and checksum a row exactly as `dirty_bands` does.
    fn hash_row(line: &[u8], seed: u32) -> u32 {
        let mut scratch = vec![0u8; line.len()];
        let n = sample_row(line, &mut scratch, PROBE_WINDOW, PROBE_WINDOW_STEP);
        hash_bytes(&scratch[..n], seed)
    }

    /// A row whose colour changes must hash differently even though alpha,
    /// which dominated the probe this one replaced, does not move.
    #[test]
    fn hash_row_sees_colour_not_just_alpha() {
        let mut a = vec![0u8; 7680];
        for p in a.chunks_mut(4) {
            p[0] = 0xff; // alpha, constant across a real desktop
        }
        let mut b = a.clone();
        for p in b.chunks_mut(4) {
            p[1] = 0x40;
            p[2] = 0x80;
            p[3] = 0xc0;
        }
        assert_ne!(hash_row(&a, 0), hash_row(&b, 0), "colour change went unnoticed");
    }

    /// A change to one sampled byte is enough; identical rows must agree.
    #[test]
    fn hash_row_is_stable_and_sensitive() {
        let a = vec![0xffu8; 7680];
        assert_eq!(hash_row(&a, 0), hash_row(&a, 0));
        let mut b = a.clone();
        b[64] ^= 0xff; // inside the first sampled run
        assert_ne!(hash_row(&a, 0), hash_row(&b, 0));
    }

    /// The whole run is read, not just its first pixel -- a change at the far
    /// end of a window must still register.
    #[test]
    fn the_whole_window_is_read() {
        let a = vec![0xffu8; 7680];
        let mut b = a.clone();
        b[PROBE_WINDOW - 1] ^= 0xff;
        assert_ne!(hash_row(&a, 0), hash_row(&b, 0), "the tail of a window went unread");
    }

    /// The blind spot, asserted rather than hoped for: this is a sampled
    /// checksum, and a change confined to the gap between two windows is missed
    /// until something else in the band moves. `session`'s settle repaint is
    /// what bounds how long that can last.
    #[test]
    fn a_change_between_windows_is_missed() {
        let a = vec![0xffu8; 7680];
        let mut b = a.clone();
        b[PROBE_WINDOW + 8] ^= 0xff;
        assert_eq!(hash_row(&a, 0), hash_row(&b, 0), "the gap is documented as unread");
    }

    /// A quarter of each sampled row is read. Stated as a test because both the
    /// cost and the sensitivity of the probe follow directly from this ratio,
    /// and because the probe it replaced covered a sixteenth for more than
    /// twice the price -- a regression in either direction should be a
    /// deliberate act.
    #[test]
    fn a_quarter_of_a_sampled_row_is_covered() {
        assert_eq!(PROBE_WINDOW * 4, PROBE_WINDOW_STEP);
    }

    /// The staged copy has to be the sampled bytes and nothing else: a short
    /// destination must truncate rather than wrap round or read past the row.
    #[test]
    fn sampling_packs_only_the_windows_it_read() {
        let line: Vec<u8> = (0..32u8).collect();
        let mut dst = [0u8; 32];
        let n = sample_row(&line, &mut dst, 4, 8);
        assert_eq!(n, 16, "four rows of four bytes, every eight");
        assert_eq!(&dst[..n], &[0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27]);

        let mut small = [0u8; 6];
        let n = sample_row(&line, &mut small, 4, 8);
        assert_eq!(n, 6, "a short destination truncates");
        assert_eq!(&small[..], &[0, 1, 2, 3, 8, 9]);
    }

    /// A nonsense pattern must return rather than loop forever: `window` and
    /// `step` reach here from `--probe-display`'s sweep.
    #[test]
    fn a_zero_window_or_step_samples_nothing() {
        let line = vec![0xffu8; 64];
        let mut dst = [0u8; 64];
        assert_eq!(sample_row(&line, &mut dst, 0, 8), 0);
        assert_eq!(sample_row(&line, &mut dst, 8, 0), 0);
    }
}

/// The main display's size right now, without building a `Capturer`.
///
/// Used when a peer logs in: the size the agent saw at startup may be minutes
/// or hours stale by then, and `PeerInfo` is what sizes the peer's canvas.
#[cfg(target_os = "macos")]
pub fn display_size() -> Option<(i32, i32)> {
    unsafe {
        let d = CGMainDisplayID();
        let (w, h) = (CGDisplayPixelsWide(d), CGDisplayPixelsHigh(d));
        if w == 0 || h == 0 {
            None
        } else {
            Some((w as i32, h as i32))
        }
    }
}

#[cfg(target_os = "macos")]
pub struct Capturer {
    display: CGDirectDisplayID,
    pub width: usize,
    pub height: usize,
    bytes_per_row: usize,
    base: *mut c_void,
    /// RAM shadow of the framebuffer; see the module note on VRAM read cost.
    buf: Vec<u8>,
    /// Per-band checksums from the last probe, for change detection.
    sums: Vec<u32>,
    /// One row's worth of staging for the probe, so the sampled runs are
    /// `memcpy`'d out of VRAM and hashed from RAM. See [`PROBE_WINDOW`].
    scratch: Vec<u8>,
}

#[cfg(target_os = "macos")]
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
                scratch: vec![0; bytes_per_row],
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
    /// the previous probe, for a small fraction of what a full read costs.
    ///
    /// This is a sampled checksum, not a proof: a change confined entirely to
    /// unsampled rows, or to the gaps between sampled runs within a row, is
    /// missed until something else in the band moves. That is the right trade
    /// for a screen-sharing agent, where the alternative is reading everything
    /// every time, and `session`'s settle repaint bounds how long a missed
    /// change can survive.
    pub fn dirty_bands(&mut self) -> [bool; BANDS] {
        self.dirty_bands_tuned(PROBE_WINDOW, PROBE_WINDOW_STEP)
    }

    /// `dirty_bands` with the sampling pattern spelled out, so `--probe-display`
    /// can time several without rebuilding the agent.
    ///
    /// Worth having as a knob rather than a constant. Whether the probe is
    /// bound by transaction count or by bytes decides the window size, that is
    /// a property of the machine's bus rather than something to reason out, and
    /// reasoning it out is exactly how it was got wrong the first time.
    pub fn dirty_bands_tuned(&mut self, window: usize, step: usize) -> [bool; BANDS] {
        let mut dirty = [false; BANDS];
        let rows_per = self.band_rows();
        let base = current_base(self.display, self.base);
        if self.scratch.len() < self.bytes_per_row {
            self.scratch.resize(self.bytes_per_row, 0);
        }
        for b in 0..BANDS {
            let start = b * rows_per;
            let end = ((b + 1) * rows_per).min(self.height);
            let mut sum: u32 = 0;
            let mut row = start;
            while row < end {
                let off = row * self.bytes_per_row;
                let line = unsafe {
                    std::slice::from_raw_parts(base.add(off), self.bytes_per_row)
                };
                let n = sample_row(line, &mut self.scratch, window, step);
                sum = hash_bytes(&self.scratch[..n], sum);
                row += PROBE_ROW_STEP;
            }
            if sum != self.sums[b] {
                dirty[b] = true;
                self.sums[b] = sum;
            }
        }
        dirty
    }

    /// Bytes a probe with this sampling pattern reads out of VRAM.
    ///
    /// Deliberately next to `dirty_bands_tuned`, walking the rows the same way,
    /// so the two cannot drift apart. Its only caller is `--probe-display`,
    /// which needs it to turn a time into a transfer rate.
    pub fn probe_bytes(&self, window: usize, step: usize) -> usize {
        if step == 0 {
            return 0;
        }
        let mut per_row = 0;
        let mut off = 0;
        while off < self.bytes_per_row {
            per_row += window.min(self.bytes_per_row - off);
            off += step;
        }
        let mut rows = 0;
        for b in 0..BANDS {
            let (start, end) = self.band_range(b);
            let mut row = start;
            while row < end {
                rows += 1;
                row += PROBE_ROW_STEP;
            }
        }
        per_row * rows
    }

    /// The rows a band covers, clamped to the screen.
    pub fn band_range(&self, b: usize) -> (usize, usize) {
        let rows_per = self.band_rows();
        let start = (b * rows_per).min(self.height);
        let end = ((b + 1) * rows_per).min(self.height);
        (start, end)
    }

    /// Copy one band out of VRAM into the shadow, returning the rows it covers.
    ///
    /// Reading a band at a time rather than all of them at once is what lets the
    /// caller service input in between: a full-screen read is ~347 ms during
    /// which nothing else happens, while one band is ~22 ms.
    pub fn read_band(&mut self, b: usize) -> (usize, usize) {
        let n = self.bytes_per_row * self.height;
        if self.buf.len() != n {
            self.buf.resize(n, 0);
        }
        let (start, end) = self.band_range(b);
        if start >= end {
            return (start, end);
        }
        let off = start * self.bytes_per_row;
        let len = (end - start) * self.bytes_per_row;
        let base = current_base(self.display, self.base);
        unsafe {
            std::ptr::copy_nonoverlapping(base.add(off), self.buf.as_mut_ptr().add(off), len);
        }
        (start, end)
    }

    /// The RAM shadow, as last read.
    pub fn buffer(&self) -> &[u8] {
        &self.buf
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
            *s = u32::MAX;
        }
    }

    /// Re-read the geometry, because the user may have changed resolution.
    ///
    /// **This has to be called before every read.** The copy length comes from
    /// the cached `bytes_per_row * height`; if the screen has since become
    /// smaller, that reads past the end of the mapping, which is a segfault
    /// rather than a wrong picture.
    ///
    /// Returns true only when the *geometry* moved, meaning the encoder and the
    /// I420 buffers have to be rebuilt around the new size. A base address that
    /// moves on its own is not interesting -- it is picked up silently.
    pub fn refresh(&mut self) -> bool {
        unsafe {
            let (w, h, bpr) = (
                CGDisplayPixelsWide(self.display),
                CGDisplayPixelsHigh(self.display),
                CGDisplayBytesPerRow(self.display),
            );
            let base = CGDisplayBaseAddress(self.display);
            if !base.is_null() {
                self.base = base;
            }
            // A geometry read can come back as zeros if the window server is
            // momentarily unreachable. Keeping the old values is right: they
            // describe a mapping that still exists, and a zero-sized frame would
            // just break the encoder.
            if w == 0 || h == 0 || bpr == 0 {
                return false;
            }
            let changed = w != self.width || h != self.height || bpr != self.bytes_per_row;
            if changed {
                self.width = w;
                self.height = h;
                self.bytes_per_row = bpr;
                self.sums = vec![u32::MAX; BANDS];
            }
            changed
        }
    }

    /// Colour depth right now. The converter assumes 32, and this vintage can
    /// be switched to 16 from the Displays preference pane.
    pub fn bits_per_pixel(&self) -> usize {
        unsafe { CGDisplayBitsPerPixel(self.display) }
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
