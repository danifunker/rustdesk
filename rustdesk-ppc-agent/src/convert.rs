//! Framebuffer ARGB -> I420, the format VP8 wants.
//!
//! Hand-written rather than libyuv, for one specific reason: **libyuv's format
//! names describe little-endian word layout, not memory order.** Its `ARGB` is
//! B,G,R,A in memory; ours is genuinely A,R,G,B (verified on the G5 — the first
//! pixels of a light-grey desktop read `ff db dd df`). Calling `ARGBToI420` here
//! would silently swap red and blue, which is exactly the class of bug that is
//! miserable to find over a remote display.
//!
//! BT.601 studio-swing coefficients, matching what libvpx/RustDesk expect:
//!
//! ```text
//!   Y =  ( 66R + 129G +  25B + 128) >> 8 +  16
//!   U =  (-38R -  74G + 112B + 128) >> 8 + 128
//!   V =  (112R -  94G -  18B + 128) >> 8 + 128
//! ```
//!
//! Chroma is 2x2 subsampled by averaging the four source pixels, which is both
//! more correct than point-sampling and cheap.

/// Planar I420: full-res Y, quarter-res U and V.
pub struct I420 {
    pub y: Vec<u8>,
    pub u: Vec<u8>,
    pub v: Vec<u8>,
    pub width: usize,
    pub height: usize,
}

impl I420 {
    /// Allocates for `width` x `height`. Both are rounded **down** to even, since
    /// 4:2:0 has no meaning for an odd trailing row or column.
    pub fn new(width: usize, height: usize) -> Self {
        let (w, h) = (width & !1, height & !1);
        Self {
            y: vec![0; w * h],
            u: vec![0; (w / 2) * (h / 2)],
            v: vec![0; (w / 2) * (h / 2)],
            width: w,
            height: h,
        }
    }

    pub fn chroma_stride(&self) -> usize {
        self.width / 2
    }
}

#[inline(always)]
fn clamp_u8(v: i32) -> u8 {
    if v < 0 {
        0
    } else if v > 255 {
        255
    } else {
        v as u8
    }
}

/// Convert a framebuffer with A,R,G,B memory order into `dst`.
///
/// `src` is the raw framebuffer, `stride` its bytes-per-row (which may exceed
/// `width * 4`; on the measured G5 it does not, but a different mode could).
pub fn argb_to_i420(src: &[u8], stride: usize, dst: &mut I420) {
    let h = dst.height;
    argb_to_i420_rows(src, stride, dst, 0, h);
}

/// Convert only rows `y0..y1`, leaving the rest of `dst` alone.
///
/// The point is latency, not throughput. Converting the whole frame costs
/// ~172 ms on the G5 whether one band changed or all sixteen, and nothing reads
/// input while it runs -- so hovering over the Dock, whose magnification
/// animates a single band, used to cost a full conversion per frame and stall
/// the mouse. Bounds are snapped outward to even rows because 4:2:0 chroma is
/// shared by each 2x2 block, so a band boundary cannot fall inside one.
pub fn argb_to_i420_rows(src: &[u8], stride: usize, dst: &mut I420, y0: usize, y1: usize) {
    #[cfg(target_os = "macos")]
    {
        // The C shim, for the optimiser -- see `convert_shim.c`. Byte-for-byte
        // identical to the Rust below, which `--probe-display` checks against a
        // real frame on the machine itself.
        let cs = dst.chroma_stride() as std::os::raw::c_int;
        let (w, h) = (dst.width as std::os::raw::c_int, dst.height as std::os::raw::c_int);
        unsafe {
            rd_argb_to_i420_rows(
                src.as_ptr(),
                src.len(),
                stride as std::os::raw::c_int,
                dst.y.as_mut_ptr(),
                dst.u.as_mut_ptr(),
                dst.v.as_mut_ptr(),
                w,
                h,
                cs,
                y0 as std::os::raw::c_int,
                y1 as std::os::raw::c_int,
            );
        }
    }
    #[cfg(not(target_os = "macos"))]
    argb_to_i420_rows_rust(src, stride, dst, y0, y1);
}

#[cfg(target_os = "macos")]
extern "C" {
    fn rd_argb_to_i420_rows(
        src: *const u8,
        src_len: usize,
        stride: std::os::raw::c_int,
        y: *mut u8,
        u: *mut u8,
        v: *mut u8,
        width: std::os::raw::c_int,
        height: std::os::raw::c_int,
        chroma_stride: std::os::raw::c_int,
        y0: std::os::raw::c_int,
        y1: std::os::raw::c_int,
    );
}

/// The reference implementation, and what the host tests exercise.
///
/// Kept even where the C shim is used: it is the definition of what the shim
/// must produce, and `--probe-display` compares the two on the target rather
/// than taking the port on trust.
pub fn argb_to_i420_rows_rust(src: &[u8], stride: usize, dst: &mut I420, y0: usize, y1: usize) {
    // Which byte of a captured pixel is which channel.
    //
    // The module header is written for the Mac's A,R,G,B, which is what this
    // was ported from. **IRIX's ReadDisplay hands back A,B,G,R** -- measured on
    // the machine with a solid red root window, where byte 3 is lit and byte 1
    // never is -- so reading it as the Mac's order swaps red and blue on every
    // pixel. That is the exact failure this module's header warns about libyuv
    // for, and it was live here.
    //
    // On the IRIX video path this function is no longer the one that runs:
    // `Capturer::to_i420_rect` fuses the conversion with the downscale and does
    // its own channel handling. This stays correct because it is the reference
    // implementation, and because `pipeline` and `--probe-display` still call it.
    #[cfg(target_os = "irix")]
    const CH: [usize; 3] = [3, 2, 1];
    #[cfg(not(target_os = "irix"))]
    const CH: [usize; 3] = [1, 2, 3];
    let (w, h) = (dst.width, dst.height);
    let cs = dst.chroma_stride();
    debug_assert!(stride >= w * 4);

    let y0 = y0 & !1;
    let y1 = ((y1 + 1) & !1).min(h);
    if y0 >= y1 {
        return;
    }
    debug_assert!(src.len() >= stride * y1);

    // One pass over 2x2 blocks: each source pixel is read once and contributes
    // to both its own luma and the block's chroma average. Measured on the G5
    // this is no faster than two passes (193 vs 196 ms) -- reads from the RAM
    // shadow are cheap, and the cost is arithmetic plus bounds checks -- but it
    // is the simpler shape, so it stays.
    for by in (y0 / 2)..(y1 / 2) {
        let (r0, r1) = (by * 2 * stride, (by * 2 + 1) * stride);
        for bx in 0..w / 2 {
            let x = bx * 2 * 4;
            let mut acc = [0i32; 3];
            let mut lum = [0u8; 4];
            for (i, (row, dx)) in [(r0, 0), (r0, 4), (r1, 0), (r1, 4)].iter().enumerate() {
                let p = &src[row + x + dx..row + x + dx + 4];
                let (r, g, b) = (p[CH[0]] as i32, p[CH[1]] as i32, p[CH[2]] as i32);
                acc[0] += r;
                acc[1] += g;
                acc[2] += b;
                lum[i] = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
            let yi = by * 2 * w + bx * 2;
            dst.y[yi] = lum[0];
            dst.y[yi + 1] = lum[1];
            dst.y[yi + w] = lum[2];
            dst.y[yi + w + 1] = lum[3];

            let (r, g, b) = (acc[0] / 4, acc[1] / 4, acc[2] / 4);
            dst.u[by * cs + bx] = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            dst.v[by * cs + bx] = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a `w`x`h` framebuffer in A,R,G,B memory order.
    fn fb(w: usize, h: usize, px: &[(u8, u8, u8)]) -> Vec<u8> {
        let mut v = Vec::with_capacity(w * h * 4);
        for i in 0..w * h {
            let (r, g, b) = px[i % px.len()];
            v.extend_from_slice(&[0xFF, r, g, b]);
        }
        v
    }

    #[test]
    fn solid_colours_land_on_the_expected_bt601_values() {
        // (r,g,b) -> (y, u, v), from the coefficients above.
        let cases: &[((u8, u8, u8), u8, u8, u8)] = &[
            ((0, 0, 0), 16, 128, 128),        // black
            ((255, 255, 255), 235, 128, 128), // white
            ((255, 0, 0), 82, 90, 240),       // red
            ((0, 255, 0), 145, 54, 34),       // green
            ((0, 0, 255), 41, 240, 110),      // blue
        ];
        for &(rgb, ey, eu, ev) in cases {
            let src = fb(4, 4, &[rgb]);
            let mut d = I420::new(4, 4);
            argb_to_i420(&src, 16, &mut d);
            assert!(d.y.iter().all(|&y| (y as i32 - ey as i32).abs() <= 1), "Y for {:?}: got {}", rgb, d.y[0]);
            assert!(d.u.iter().all(|&u| (u as i32 - eu as i32).abs() <= 1), "U for {:?}: got {}", rgb, d.u[0]);
            assert!(d.v.iter().all(|&v| (v as i32 - ev as i32).abs() <= 1), "V for {:?}: got {}", rgb, d.v[0]);
        }
    }

    /// The bug this module exists to avoid: reading the buffer as B,G,R,A would
    /// turn pure red into pure blue.
    #[test]
    fn red_is_not_blue() {
        let red = fb(2, 2, &[(255, 0, 0)]);
        let blue = fb(2, 2, &[(0, 0, 255)]);
        let (mut dr, mut db) = (I420::new(2, 2), I420::new(2, 2));
        argb_to_i420(&red, 8, &mut dr);
        argb_to_i420(&blue, 8, &mut db);
        // Red has high V and low U; blue is the reverse.
        assert!(dr.v[0] > 200 && dr.u[0] < 120, "red: u={} v={}", dr.u[0], dr.v[0]);
        assert!(db.u[0] > 200 && db.v[0] < 120, "blue: u={} v={}", db.u[0], db.v[0]);
    }

    #[test]
    fn alpha_byte_is_ignored() {
        let mut a = fb(2, 2, &[(10, 20, 30)]);
        let b = a.clone();
        for i in (0..a.len()).step_by(4) {
            a[i] = 0x00; // vary only the alpha/unused byte
        }
        let (mut da, mut db) = (I420::new(2, 2), I420::new(2, 2));
        argb_to_i420(&a, 8, &mut da);
        argb_to_i420(&b, 8, &mut db);
        assert_eq!(da.y, db.y);
        assert_eq!(da.u, db.u);
    }

    #[test]
    fn honours_a_stride_larger_than_the_visible_width() {
        // 2 visible pixels per row, but 4 pixels' worth of stride.
        let w = 2usize;
        let stride = 4 * 4;
        let mut src = vec![0u8; stride * 2];
        for row in 0..2 {
            for col in 0..w {
                let o = row * stride + col * 4;
                src[o..o + 4].copy_from_slice(&[0xFF, 255, 255, 255]); // white, visible
            }
            for col in w..4 {
                let o = row * stride + col * 4;
                src[o..o + 4].copy_from_slice(&[0xFF, 255, 0, 0]); // red, past the edge
            }
        }
        let mut d = I420::new(w, 2);
        argb_to_i420(&src, stride, &mut d);
        // Only the white pixels should have been read.
        assert!(d.y.iter().all(|&y| y >= 234), "padding leaked into Y: {:?}", d.y);
    }

    #[test]
    fn odd_dimensions_round_down_to_even() {
        let d = I420::new(7, 5);
        assert_eq!((d.width, d.height), (6, 4));
        assert_eq!(d.y.len(), 24);
        assert_eq!(d.u.len(), 6);
        assert_eq!(d.v.len(), 6);
    }

    #[test]
    fn chroma_averages_the_2x2_block() {
        // One white pixel among three black: chroma should sit near neutral,
        // luma near a quarter of full scale.
        let mut src = fb(2, 2, &[(0, 0, 0)]);
        src[0..4].copy_from_slice(&[0xFF, 255, 255, 255]);
        let mut d = I420::new(2, 2);
        argb_to_i420(&src, 8, &mut d);
        assert_eq!(d.u.len(), 1);
        assert!((d.u[0] as i32 - 128).abs() <= 2, "u={}", d.u[0]);
        assert!((d.v[0] as i32 - 128).abs() <= 2, "v={}", d.v[0]);
        assert!(d.y[0] > 200 && d.y[3] < 20, "luma not per-pixel: {:?}", d.y);
    }
}

#[cfg(test)]
mod band_tests {
    use super::*;

    fn frame(w: usize, h: usize, seed: u8) -> Vec<u8> {
        let mut v = vec![0u8; w * h * 4];
        for (i, p) in v.chunks_mut(4).enumerate() {
            p[0] = 0xff;
            p[1] = (i as u8).wrapping_mul(3).wrapping_add(seed);
            p[2] = (i as u8).wrapping_mul(5).wrapping_add(seed);
            p[3] = (i as u8).wrapping_mul(7).wrapping_add(seed);
        }
        v
    }

    /// Converting in bands must produce exactly what one whole-frame pass does,
    /// or a partially updated frame would drift from the real screen.
    #[test]
    fn band_by_band_equals_a_single_full_conversion() {
        let (w, h) = (64usize, 32usize);
        let src = frame(w, h, 11);
        let mut whole = I420::new(w, h);
        argb_to_i420(&src, w * 4, &mut whole);

        let mut banded = I420::new(w, h);
        for b in 0..8 {
            let (y0, y1) = (b * 4, (b + 1) * 4);
            argb_to_i420_rows(&src, w * 4, &mut banded, y0, y1);
        }
        assert_eq!(banded.y, whole.y, "luma differs");
        assert_eq!(banded.u, whole.u, "chroma u differs");
        assert_eq!(banded.v, whole.v, "chroma v differs");
    }

    /// Rows outside the range must not be touched: the rest of the plane still
    /// holds the previous frame, which is the whole point of reading bands.
    #[test]
    fn rows_outside_the_range_are_left_alone() {
        let (w, h) = (16usize, 16usize);
        let mut img = I420::new(w, h);
        for b in img.y.iter_mut() {
            *b = 0xAB;
        }
        let src = frame(w, h, 3);
        argb_to_i420_rows(&src, w * 4, &mut img, 4, 8);
        assert!(img.y[0..4 * w].iter().all(|b| *b == 0xAB), "rows above were overwritten");
        assert!(img.y[8 * w..].iter().all(|b| *b == 0xAB), "rows below were overwritten");
        assert!(img.y[4 * w..8 * w].iter().any(|b| *b != 0xAB), "the range was not converted");
    }

    /// An odd range must still land on whole 2x2 blocks.
    #[test]
    fn an_odd_range_is_snapped_to_even_rows() {
        let (w, h) = (16usize, 16usize);
        let src = frame(w, h, 5);
        let mut a = I420::new(w, h);
        let mut b = I420::new(w, h);
        argb_to_i420_rows(&src, w * 4, &mut a, 3, 7);
        argb_to_i420_rows(&src, w * 4, &mut b, 2, 8);
        assert_eq!(a.y, b.y);
        assert_eq!(a.u, b.u);
    }

    #[test]
    fn an_empty_or_inverted_range_does_nothing() {
        let (w, h) = (8usize, 8usize);
        let src = frame(w, h, 1);
        let mut img = I420::new(w, h);
        argb_to_i420_rows(&src, w * 4, &mut img, 4, 4);
        argb_to_i420_rows(&src, w * 4, &mut img, 6, 2);
        assert!(img.y.iter().all(|b| *b == 0), "nothing should have been written");
    }
}
