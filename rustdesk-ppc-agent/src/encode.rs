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
}

/// Fastest setting VP8 accepts. Quality is not the constraint here; wall-clock is.
const CPU_USED: c_int = -16;

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
        // Asked for at runtime, never assumed: this has to run on
        // single-processor G4s and G5s as well as the dual G5 it is developed
        // on. Two is the useful ceiling for VP8 at this resolution -- more
        // partitions cost bitstream overhead for work there are no cores to do.
        let threads = crate::sys::threads_for(2);
        log::info!(
            "encoder: {}x{}, {} kbps, {} thread(s) of {} processor(s)",
            width,
            height,
            bitrate_kbps,
            threads,
            crate::sys::cpu_count()
        );
        let inner = unsafe {
            vpxenc_new(
                width as c_int,
                height as c_int,
                bitrate_kbps as c_int,
                CPU_USED,
                threads as c_int,
            )
        };
        if inner.is_null() {
            return Err("vpx encoder init failed");
        }
        Ok(Self { inner, width, height })
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
