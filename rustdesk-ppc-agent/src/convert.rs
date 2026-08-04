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
    let (w, h) = (dst.width, dst.height);
    let cs = dst.chroma_stride();
    debug_assert!(stride >= w * 4);
    debug_assert!(src.len() >= stride * h);

    // One pass over 2x2 blocks: each source pixel is read once and contributes
    // to both its own luma and the block's chroma average. Measured on the G5
    // this is no faster than two passes (193 vs 196 ms) -- reads from the RAM
    // shadow are cheap, and the cost is arithmetic plus bounds checks -- but it
    // is the simpler shape, so it stays.
    for by in 0..h / 2 {
        let (r0, r1) = (by * 2 * stride, (by * 2 + 1) * stride);
        for bx in 0..w / 2 {
            let x = bx * 2 * 4;
            let mut acc = [0i32; 3];
            let mut lum = [0u8; 4];
            for (i, (row, dx)) in [(r0, 0), (r0, 4), (r1, 0), (r1, 4)].iter().enumerate() {
                let p = &src[row + x + dx..row + x + dx + 4];
                let (r, g, b) = (p[1] as i32, p[2] as i32, p[3] as i32);
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
