//! VP8 encoding, over the C shim in `vpx_shim.c`.
//!
//! Only scalars and byte buffers cross the FFI boundary — `vpx_codec_enc_cfg_t`
//! stays entirely on the C side, so there is no struct layout to get wrong on a
//! 32-bit big-endian target. See the shim's header comment.
//!
//! VP8 rather than VP9: it is markedly cheaper to encode, and this hardware has
//! no headroom. The encoder runs with the realtime deadline and the fastest
//! `cpu_used` the codec accepts.

use std::os::raw::{c_int, c_longlong};

use crate::convert::I420;

#[repr(C)]
struct VpxEnc {
    _private: [u8; 0],
}

extern "C" {
    fn vpxenc_new(
        width: c_int,
        height: c_int,
        bitrate_kbps: c_int,
        cpu_used: c_int,
        threads: c_int,
        static_thresh: c_int,
        last_ref_only: c_int,
        error_resilient: c_int,
        profile: c_int,
        min_q: c_int,
        screen_content: c_int,
        auto_keyframes: c_int,
    ) -> *mut VpxEnc;
    fn vpxenc_encode(
        e: *mut VpxEnc,
        y: *const u8,
        u: *const u8,
        v: *const u8,
        ystride: c_int,
        ustride: c_int,
        vstride: c_int,
        pts_ms: c_longlong,
        force_key: c_int,
        data: *mut *const u8,
        len: *mut usize,
        is_key: *mut c_int,
    ) -> c_int;
    fn vpxenc_free(e: *mut VpxEnc);
    fn vpxenc_mb_rows(e: *const VpxEnc) -> c_int;
    fn vpxenc_mb_cols(e: *const VpxEnc) -> c_int;
    fn vpxenc_amap_clear(e: *mut VpxEnc);
    fn vpxenc_amap_rect(e: *mut VpxEnc, x: c_int, y: c_int, w: c_int, h: c_int);
    fn vpxenc_amap_off(e: *mut VpxEnc);
    fn vpxenc_amap_active(e: *const VpxEnc) -> c_int;
}

/// Fastest setting VP8 accepts. Quality is not the constraint here; wall-clock is.
const CPU_USED: c_int = -16;

/// The knobs that trade picture for wall-clock, gathered so they can be swept.
///
/// A whole-frame VP8 encode is the floor for an ordinary interactive update --
/// it costs the same whether one line of text moved or the entire screen did --
/// so these are aimed squarely at the per-macroblock work that a mostly-static
/// desktop should not have to pay for. `--probe-display` times each of them
/// against both a still frame and a small change. Nothing here changes the
/// bitstream's decodability, so a client cannot tell which settings were used.
///
/// Measured on the dual G5 at 1920x1080, median of three steady-state frames:
///
/// ```text
///   threshold  1000   48 ms    threshold 15000   30 ms
///   threshold  6000   39 ms    threshold 30000   29 ms
/// ```
///
/// The threshold is the only one that moves the clock -- and it is not free
/// speed, which is why the default sits at the slow end of that table. See
/// `Default`. The other two are kept as knobs, and as recorded
/// negative results: predicting from the last frame alone measured 47 ms, i.e.
/// nothing, because at `cpu_used = -16` VP8's fast mode picker was never
/// searching golden and altref anyway; and dropping error resilience measured
/// nothing either *and* made frames bigger (1706 against 1566 bytes), so the
/// entropy update it enables is apparently not what carries the cost here. Both
/// stay switchable because this agent targets a family of machines and the
/// sweep is how the next one gets checked rather than assumed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Tune {
    /// `VP8E_SET_STATIC_THRESHOLD`: below this the macroblock is coded as skip.
    pub static_threshold: u32,
    /// Predict only from the previous frame, never golden or altref.
    pub last_ref_only: bool,
    /// Code each frame independently of the entropy state. Pointless over TCP.
    pub error_resilient: bool,
    /// VP8 bitstream profile, 0-3. **Not a feature level** -- every VP8 decoder
    /// must handle all four -- but a set of encoder cost decisions:
    ///
    /// ```text
    ///   0  normal loop filter, six-tap sub-pixel MC   (libvpx default)
    ///   1  simple loop filter, bilinear MC
    ///   2  no loop filter,     bilinear MC
    ///   3  no loop filter,     simple filter, full-pixel MC only
    /// ```
    ///
    /// The loop filter is a pass over every pixel of every frame, and its job
    /// is hiding block edges in natural video. See `Default` for what it
    /// measured here.
    pub profile: u32,
    /// `rc_min_quantizer`: the floor on quality, and so on how many
    /// coefficients there are to transform, quantise and tokenise.
    pub min_q: u32,
    /// `VP8E_SET_SCREEN_CONTENT_MODE`, 0-2. Tells VP8 it is looking at a
    /// desktop rather than a camera, which turns off three heuristics that are
    /// wrong here — the dot-artifact check, the skin-map lookup, and a ZEROMV
    /// bias tuned for natural video — and, at 2, keeps a golden frame updated
    /// for the static parts.
    pub screen_content: u32,
    /// Let libvpx put in key frames of its own: every 128 frames (its
    /// `kf_max_dist` default) and wherever it decides the picture has cut.
    /// The session already sends one when a peer arrives, asks, or changes
    /// scale, and schedules its own backstop (`session::KEYFRAME_FRAMES`), so
    /// these are extra -- and they are the most expensive frame there is. On an
    /// O2 a 1280x1024 key frame costs 2.3 s against 46 ms for a pointer move,
    /// a stall every 128 frames of real use. Upstream RustDesk turns them off
    /// (`VPX_KF_DISABLED`, "reduce bandwidth a lot"); nothing is lost over TCP.
    pub auto_keyframes: bool,
}

impl Default for Tune {
    fn default() -> Self {
        Self {
            // **1000, and the timings above are why this is not higher.**
            // 15000 measured 18 ms faster and looked like free speed. It is
            // not: a macroblock whose error falls under the threshold is not
            // coded *at all*, and the bottom sliver of a line of text lands in
            // a macroblock that is mostly background, so its error is small
            // even though the change is perfectly visible. Reported from a real
            // session as characters arriving in halves -- "the horizontal part
            // of the t but not the bottom part" -- with the rest appearing
            // whenever something later pushed those blocks over the threshold.
            //
            // The reasoning that picked 15000 was that high-contrast changes
            // are nowhere near the threshold. True of a whole glyph, false of
            // the third of one that shares a macroblock with blank paper, and
            // text is what this agent is mostly used to look at. 18 ms is not
            // worth reading half a character.
            static_threshold: 1000,
            last_ref_only: false,
            error_resilient: true,
            profile: 0,
            min_q: 8,
            screen_content: 0,
            auto_keyframes: true,
        }
    }
}

pub struct Encoder {
    inner: *mut VpxEnc,
    pub width: usize,
    pub height: usize,
}

pub struct Frame<'a> {
    pub data: &'a [u8],
    pub key: bool,
    pub pts_ms: i64,
}

impl Encoder {
    pub fn new(width: usize, height: usize, bitrate_kbps: u32) -> Result<Self, &'static str> {
        Self::tuned(width, height, bitrate_kbps, Tune::default())
    }

    pub fn tuned(
        width: usize,
        height: usize,
        bitrate_kbps: u32,
        tune: Tune,
    ) -> Result<Self, &'static str> {
        // Asked for at runtime, never assumed: this has to run on
        // single-processor G4s and G5s as well as the dual G5 it is developed
        // on. Two is the useful ceiling for VP8 at this resolution -- more
        // partitions cost bitstream overhead for work there are no cores to do.
        let threads = crate::sys::threads_for(2);
        log::info!(
            "encoder: {}x{}, {} kbps, {} thread(s) of {} processor(s), {:?}",
            width,
            height,
            bitrate_kbps,
            threads,
            crate::sys::cpu_count(),
            tune
        );
        let inner = unsafe {
            vpxenc_new(
                width as c_int,
                height as c_int,
                bitrate_kbps as c_int,
                CPU_USED,
                threads as c_int,
                tune.static_threshold as c_int,
                tune.last_ref_only as c_int,
                tune.error_resilient as c_int,
                tune.profile as c_int,
                tune.min_q as c_int,
                tune.screen_content as c_int,
                tune.auto_keyframes as c_int,
            )
        };
        if inner.is_null() {
            return Err("vpx encoder init failed");
        }
        Ok(Self { inner, width, height })
    }

    /// Macroblocks across and down. The active map is one byte per macroblock,
    /// so these are what a caller needs to reason about its granularity: on a
    /// 640x512 frame the whole screen is 40 x 32 = 1280 of them, and a line of
    /// text in a terminal is about six.
    pub fn mb_rows(&self) -> usize {
        unsafe { vpxenc_mb_rows(self.inner) as usize }
    }

    pub fn mb_cols(&self) -> usize {
        unsafe { vpxenc_mb_cols(self.inner) as usize }
    }

    /// Begin describing which macroblocks the next inter frame should look at.
    /// Everything is inactive until `active_rect` says otherwise.
    ///
    /// **Only sound on top of a damage report that cannot miss a change.** An
    /// inactive macroblock keeps the previous frame's pixels indefinitely, so a
    /// change that goes unreported is a permanent artefact rather than a late
    /// one. IRIX's SGI-SCREEN-CAPTURE reports exact rectangles; the Mac's
    /// sampled checksum does not, which is why this is not wired in there.
    pub fn active_none(&mut self) {
        unsafe { vpxenc_amap_clear(self.inner) };
    }

    /// Mark the macroblocks a rectangle of the *encoded* frame touches.
    /// Coordinates are in encoder pixels, so a caller working at a scale factor
    /// has to divide first.
    pub fn active_rect(&mut self, x: i32, y: i32, w: i32, h: i32) {
        unsafe { vpxenc_amap_rect(self.inner, x, y, w, h) };
    }

    /// Go back to considering every macroblock.
    pub fn active_all(&mut self) {
        unsafe { vpxenc_amap_off(self.inner) };
    }

    /// How many macroblocks the current map would encode, for logging. The
    /// ratio against `mb_rows() * mb_cols()` is the whole story of whether the
    /// damage report is buying anything.
    pub fn active_count(&self) -> usize {
        unsafe { vpxenc_amap_active(self.inner) as usize }
    }

    /// Encode one frame. The returned slice is owned by the encoder and stays
    /// valid until the next call.
    pub fn encode(&mut self, img: &I420, pts_ms: i64, force_key: bool) -> Result<Frame<'_>, &'static str> {
        if img.width != self.width || img.height != self.height {
            return Err("frame size does not match the encoder");
        }
        let mut data: *const u8 = std::ptr::null();
        let mut len: usize = 0;
        let mut is_key: c_int = 0;
        let rc = unsafe {
            vpxenc_encode(
                self.inner,
                img.y.as_ptr(),
                img.u.as_ptr(),
                img.v.as_ptr(),
                img.width as c_int,
                img.chroma_stride() as c_int,
                img.chroma_stride() as c_int,
                pts_ms as c_longlong,
                if force_key { 1 } else { 0 },
                &mut data,
                &mut len,
                &mut is_key,
            )
        };
        match rc {
            0 => Ok(Frame {
                data: if data.is_null() || len == 0 {
                    &[]
                } else {
                    unsafe { std::slice::from_raw_parts(data, len) }
                },
                key: is_key != 0,
                pts_ms,
            }),
            -2 => Err("vpx_codec_encode failed"),
            -3 => Err("out of memory collecting encoded packets"),
            _ => Err("invalid arguments to the encoder"),
        }
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { vpxenc_free(self.inner) };
        self.inner = std::ptr::null_mut();
    }
}

// The encoder is only used from the single session thread.
unsafe impl Send for Encoder {}


/// A VP8 decoder, for checking the picture rather than the pipeline.
///
/// Every stage of the encode path has been verified against something that
/// agrees with it — the converter against its own reference, the encoder
/// against its own round trip — and none of that answers whether the frame the
/// peer receives looks like the screen. A red/blue swap satisfies every one of
/// those checks, and did, for two sessions.
///
/// Used by `testpeer`, not by the agent. It exists so that a change to the
/// encoder's settings can be checked by looking at the result: dropping the
/// loop filter and sub-pixel motion compensation (see [`Tune::profile`]) leaves
/// a perfectly decodable stream whether or not it still resembles the desktop.
pub struct Decoder {
    inner: *mut VpxDec,
    /// Last decoded geometry, so a caller can size its planes.
    pub width: usize,
    pub height: usize,
}

#[repr(C)]
struct VpxDec {
    _private: [u8; 0],
}

extern "C" {
    fn vpxdec_new() -> *mut VpxDec;
    fn vpxdec_decode(
        d: *mut VpxDec,
        data: *const u8,
        len: usize,
        y: *mut u8,
        u: *mut u8,
        v: *mut u8,
        ycap: usize,
        uvcap: usize,
        w: *mut c_int,
        h: *mut c_int,
    ) -> c_int;
    fn vpxdec_free(d: *mut VpxDec);
}

impl Decoder {
    pub fn new() -> Result<Self, &'static str> {
        let inner = unsafe { vpxdec_new() };
        if inner.is_null() {
            return Err("vpx decoder init failed");
        }
        Ok(Self { inner, width: 0, height: 0 })
    }

    /// Decode one frame into `img`, resizing it if the stream says to.
    ///
    /// Frames must be fed in order and none may be skipped: an inter frame is a
    /// difference against its predecessor, so a gap does not produce a slightly
    /// wrong picture, it produces a wrong one that stays wrong until the next
    /// keyframe.
    pub fn decode(&mut self, data: &[u8], img: &mut I420) -> Result<(), &'static str> {
        let (mut w, mut h) = (0 as c_int, 0 as c_int);
        let call = |img: &mut I420, w: &mut c_int, h: &mut c_int| unsafe {
            vpxdec_decode(
                self.inner,
                data.as_ptr(),
                data.len(),
                img.y.as_mut_ptr(),
                img.u.as_mut_ptr(),
                img.v.as_mut_ptr(),
                img.y.len(),
                img.u.len(),
                w,
                h,
            )
        };
        match call(img, &mut w, &mut h) {
            0 => {}
            // -2 means the planes were too small, and it reports the size that
            // would do. Grow and decode the *same* frame again rather than
            // dropping it: see the note above about gaps.
            -2 => {
                *img = I420::new(w as usize, h as usize);
                if call(img, &mut w, &mut h) != 0 {
                    return Err("vpx decode failed after resizing");
                }
            }
            _ => return Err("vpx decode failed"),
        }
        self.width = w as usize;
        self.height = h as usize;
        Ok(())
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        unsafe { vpxdec_free(self.inner) };
        self.inner = std::ptr::null_mut();
    }
}

unsafe impl Send for Decoder {}
