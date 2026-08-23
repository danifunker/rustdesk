//! PNG, for `ScreenshotResponse.data`.
//!
//! The client offers a screenshot button to any peer claiming 1.4.0 or newer and
//! expects a PNG back -- upstream builds one with `repng`, and the client feeds
//! `response.data` straight to its image decoder. So the format is not a choice.
//!
//! Unlike [`crate::zstd_frame`], which exists because linking libzstd for this
//! target was a build problem, this links the real compressor. A PNG's `IDAT`
//! payload is exactly a zlib stream, which is exactly what `compress2` produces,
//! and zlib is not a build problem here: 10.5 ships 1.2.3 with its header in
//! `/usr/lib`, and this toolchain in fact resolves `-lz` to MacPorts' 1.3.2,
//! which the binary already depends on for `libgcc_s` and
//! `libMacportsLegacySupport` regardless. Storing the data uncompressed the way
//! `zstd_frame` does is possible -- deflate has stored blocks -- but it would
//! mean ~6 MB per press against 1.6, for more code rather than less.
//!
//! Layout (PNG spec, ISO 15948):
//!
//! ```text
//!   signature   8 bytes, 89 'P' 'N' 'G' 13 10 26 10
//!   IHDR       13 bytes: width, height, depth, colour type, 3 zero bytes
//!   IDAT        the zlib stream
//!   IEND        empty
//! ```
//!
//! Every chunk carries its length, its type, and a CRC-32 over type and data.

use libc::{c_int, c_uint, c_ulong};

// zlib. Only scalars and pointers cross this boundary, so unlike the CoreGraphics
// calls in `input_shim.c` there is no calling-convention hazard to route around.
#[link(name = "z")]
extern "C" {
    fn compress2(
        dest: *mut u8,
        dest_len: *mut c_ulong,
        src: *const u8,
        src_len: c_ulong,
        level: c_int,
    ) -> c_int;
    fn compressBound(src_len: c_ulong) -> c_ulong;
    fn crc32(crc: c_ulong, buf: *const u8, len: c_uint) -> c_ulong;
}

/// Deflate level. Measured on the G5, a real desktop at 1920x1080, via
/// `--probe-display`:
///
/// ```text
/// level 1 : 210 ms   1608 KB
/// level 6 : 387 ms   1436 KB
/// level 9 : 618 ms   1430 KB
/// ```
///
/// Level 1. The session loop is blocked for the whole encode -- no video, no
/// input -- so what is scarce here is the stall, not the bytes: 6 buys 172 KB
/// for 177 ms, on a LAN measured to move a 95 KB video frame in under 4 ms. And
/// 9 buys 6 KB for another 231 ms, which is the usual shape for screen content.
///
/// The first version of this measured 1281 ms at level 1, which is why the
/// packing moved into C -- see `pack_rgb_rows`. Almost none of that was deflate.
const LEVEL: c_int = 1;

/// PNG colour type 2: three 8-bit channels, no alpha.
///
/// The framebuffer's fourth channel carries nothing. That is measured rather
/// than assumed -- the sampling bug in `capture::hash_row` was diagnosed by
/// finding 0xff in the alpha byte of every pixel on the desktop -- so encoding
/// it would cost a quarter of the bytes to transmit a constant.
const COLOUR_TYPE_RGB: u8 = 2;

/// Encode a captured ARGB framebuffer as a PNG.
///
/// `stride` is the framebuffer's bytes per row, which is **not** `width * 4`:
/// the window server pads rows, and using the wrong one shears the image
/// diagonally.
///
/// Byte order in the source is A, R, G, B, as established for the cursor images
/// in `cursor_shim.c` by counting non-zero bytes across a real arrow. libyuv's
/// naming for the same layout is the opposite way round, which has already
/// caused one wrong turn -- see the note in `docs/performance-plan.md` §5.
pub fn encode_argb(
    fb: &[u8],
    stride: usize,
    width: usize,
    height: usize,
) -> Result<Vec<u8>, &'static str> {
    encode_argb_tuned(fb, stride, width, height, LEVEL)
}

/// `encode_argb` with the deflate level spelled out, so `--probe-display` can
/// re-measure the trade on whatever machine is in front of you rather than
/// inheriting the number from the G5 this was tuned on.
pub fn encode_argb_tuned(
    fb: &[u8],
    stride: usize,
    width: usize,
    height: usize,
    level: i32,
) -> Result<Vec<u8>, &'static str> {
    if width == 0 || height == 0 {
        return Err("zero-sized display");
    }
    if stride < width * 4 {
        return Err("stride is narrower than the row it describes");
    }
    if fb.len() < stride * height {
        return Err("framebuffer is shorter than its own geometry");
    }

    let raw = pack_rgb_rows(fb, stride, width, height);
    let idat = deflate(&raw, level as c_int)?;

    let mut out = Vec::with_capacity(idat.len() + 64);
    out.extend_from_slice(&[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]);

    let mut ihdr = Vec::with_capacity(13);
    ihdr.extend_from_slice(&(width as u32).to_be_bytes());
    ihdr.extend_from_slice(&(height as u32).to_be_bytes());
    ihdr.push(8); // bits per channel
    ihdr.push(COLOUR_TYPE_RGB);
    ihdr.extend_from_slice(&[0, 0, 0]); // deflate, adaptive filtering, no interlace
    chunk(&mut out, b"IHDR", &ihdr);
    chunk(&mut out, b"IDAT", &idat);
    chunk(&mut out, b"IEND", &[]);
    Ok(out)
}

/// Turn a captured framebuffer into PNG scanlines: a filter byte, then R,G,B per
/// pixel, per row.
///
/// Filter 0 is "None" -- the bytes go through as they are and deflate does all
/// the work. The predictive filters would help a photograph; on screen content
/// the flat regions are already long repeats that LZ77 matches.
///
/// In C on the target for the same reason `argb_to_i420_rows` is, and it is the
/// same measurement: this walks the same 8 MB framebuffer a pixel at a time, and
/// the Rust reference for that walk measures ~1.8 s under mrustc's -O1 with
/// bounds checks intact. See `convert_shim.c`.
pub fn pack_rgb_rows(fb: &[u8], stride: usize, width: usize, height: usize) -> Vec<u8> {
    let mut dst = vec![0u8; (1 + width * 3) * height];
    #[cfg(target_os = "macos")]
    unsafe {
        rd_argb_to_png_rows(
            fb.as_ptr(),
            fb.len(),
            stride as c_int,
            dst.as_mut_ptr(),
            dst.len(),
            width as c_int,
            height as c_int,
        );
    }
    #[cfg(not(target_os = "macos"))]
    pack_rgb_rows_into(fb, stride, width, height, &mut dst);
    dst
}

#[cfg(target_os = "macos")]
extern "C" {
    fn rd_argb_to_png_rows(
        src: *const u8,
        src_len: usize,
        stride: c_int,
        dst: *mut u8,
        dst_len: usize,
        width: c_int,
        height: c_int,
    );
}

/// The reference implementation, and what the host tests exercise.
///
/// Kept where the C shim is used, like `argb_to_i420_rows_rust`: it is the
/// definition of what the shim must produce, and `--probe-display` compares the
/// two on a real frame on the target rather than taking the port on trust.
pub fn pack_rgb_rows_rust(fb: &[u8], stride: usize, width: usize, height: usize) -> Vec<u8> {
    let mut dst = vec![0u8; (1 + width * 3) * height];
    pack_rgb_rows_into(fb, stride, width, height, &mut dst);
    dst
}

fn pack_rgb_rows_into(fb: &[u8], stride: usize, width: usize, height: usize, dst: &mut [u8]) {
    let row = 1 + width * 3;
    // Which byte of a captured pixel is which channel. The Mac's framebuffer is
    // A,R,G,B; IRIX's ReadDisplay hands back **A,B,G,R** -- measured, and the
    // difference is exactly a red/blue swap, which in a screenshot is a bug
    // nobody notices until they are looking at a photograph.
    #[cfg(target_os = "irix")]
    const CH: [usize; 3] = [3, 2, 1];
    #[cfg(not(target_os = "irix"))]
    const CH: [usize; 3] = [1, 2, 3];
    for y in 0..height {
        let src = &fb[y * stride..y * stride + width * 4];
        let out = &mut dst[y * row..(y + 1) * row];
        out[0] = 0; // filter type None
        for (px, o) in src.chunks_exact(4).zip(out[1..].chunks_exact_mut(3)) {
            o[0] = px[CH[0]]; // R
            o[1] = px[CH[1]]; // G
            o[2] = px[CH[2]]; // B
        }
    }
}

/// Append one chunk: length, type, data, and the CRC-32 over type and data.
fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    // The length is deliberately outside the CRC, and the type deliberately
    // inside it. zlib's crc32 is the same CRC the spec names, seeded at 0 and
    // fed in two pieces.
    let c = unsafe {
        let c = crc32(0, kind.as_ptr(), 4);
        crc32(c, data.as_ptr(), data.len() as c_uint)
    };
    out.extend_from_slice(&(c as u32).to_be_bytes());
}

/// zlib-compress `src`, which is what a PNG `IDAT` holds.
fn deflate(src: &[u8], level: c_int) -> Result<Vec<u8>, &'static str> {
    let mut cap = unsafe { compressBound(src.len() as c_ulong) };
    let mut out = vec![0u8; cap as usize];
    let rc = unsafe {
        compress2(
            out.as_mut_ptr(),
            &mut cap,
            src.as_ptr(),
            src.len() as c_ulong,
            level,
        )
    };
    if rc != 0 {
        // Z_MEM_ERROR, Z_BUF_ERROR or Z_STREAM_ERROR. Nothing here can retry
        // usefully, so the caller turns it into a message for the peer.
        return Err("zlib refused to compress the screenshot");
    }
    out.truncate(cap as usize);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A gradient with a distinct value in every channel, so a swapped or
    /// dropped channel cannot pass. Padded stride, because that is the case the
    /// real framebuffer presents and the easiest one to get wrong.
    fn fixture(width: usize, height: usize, stride: usize) -> Vec<u8> {
        let mut fb = vec![0u8; stride * height];
        for y in 0..height {
            for x in 0..width {
                let p = y * stride + x * 4;
                fb[p] = 0xff; // alpha, which must not appear in the output
                fb[p + 1] = (x * 7 + y) as u8; // R
                fb[p + 2] = (x + y * 3) as u8; // G
                fb[p + 3] = (x * 2 + y * 5) as u8; // B
            }
        }
        fb
    }

    #[test]
    fn the_header_says_what_was_encoded() {
        let png = encode_argb(&fixture(4, 3, 32), 32, 4, 3).unwrap();
        assert_eq!(&png[..8], &[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]);
        // Length, type, then the fields.
        assert_eq!(&png[8..12], &13u32.to_be_bytes());
        assert_eq!(&png[12..16], b"IHDR");
        assert_eq!(&png[16..20], &4u32.to_be_bytes());
        assert_eq!(&png[20..24], &3u32.to_be_bytes());
        assert_eq!(png[24], 8);
        assert_eq!(png[25], COLOUR_TYPE_RGB);
        assert_eq!(&png[26..29], &[0, 0, 0]);
    }

    #[test]
    fn it_ends_with_iend() {
        let png = encode_argb(&fixture(8, 8, 64), 64, 8, 8).unwrap();
        assert_eq!(&png[png.len() - 8..png.len() - 4], b"IEND");
        assert_eq!(&png[png.len() - 12..png.len() - 8], &0u32.to_be_bytes());
    }

    /// Every chunk's CRC has to check out, since a wrong one is the failure that
    /// a decoder rejects and every hand-written test here would still pass.
    #[test]
    fn every_chunk_crc_is_correct() {
        let png = encode_argb(&fixture(9, 5, 48), 48, 9, 5).unwrap();
        let mut i = 8;
        let mut kinds = Vec::new();
        while i < png.len() {
            let len = u32::from_be_bytes([png[i], png[i + 1], png[i + 2], png[i + 3]]) as usize;
            let kind = &png[i + 4..i + 8];
            let body = &png[i + 4..i + 8 + len]; // type and data together
            let want = u32::from_be_bytes([
                png[i + 8 + len],
                png[i + 9 + len],
                png[i + 10 + len],
                png[i + 11 + len],
            ]);
            let got = unsafe { crc32(0, body.as_ptr(), body.len() as c_uint) } as u32;
            assert_eq!(got, want, "CRC wrong on {:?}", std::str::from_utf8(kind));
            kinds.push(String::from_utf8_lossy(kind).into_owned());
            i += 12 + len;
        }
        assert_eq!(kinds, vec!["IHDR", "IDAT", "IEND"]);
        assert_eq!(i, png.len(), "chunks did not tile the file exactly");
    }

    /// The pixels survive the round trip, checked by inflating IDAT rather than
    /// by trusting the encoder's own arithmetic. `examples/png_check.rs` does the
    /// same against a real decoder, which is the check that would catch a
    /// spec-legal file that no library actually accepts.
    #[test]
    fn the_pixels_come_back_unswapped() {
        let (w, h, stride) = (5, 4, 40);
        let fb = fixture(w, h, stride);
        let png = encode_argb(&fb, stride, w, h).unwrap();

        // Find IDAT and inflate it. zlib's uncompress is the mirror of the call
        // that produced it, so this tests the framing rather than the codec.
        let mut i = 8;
        let idat = loop {
            let len = u32::from_be_bytes([png[i], png[i + 1], png[i + 2], png[i + 3]]) as usize;
            if &png[i + 4..i + 8] == b"IDAT" {
                break &png[i + 8..i + 8 + len];
            }
            i += 12 + len;
        };
        let mut raw = vec![0u8; h * (1 + w * 3) + 16];
        let mut n = raw.len() as c_ulong;
        extern "C" {
            fn uncompress(
                dest: *mut u8,
                dest_len: *mut c_ulong,
                src: *const u8,
                src_len: c_ulong,
            ) -> c_int;
        }
        let rc = unsafe {
            uncompress(
                raw.as_mut_ptr(),
                &mut n,
                idat.as_ptr(),
                idat.len() as c_ulong,
            )
        };
        assert_eq!(rc, 0, "IDAT did not inflate");
        raw.truncate(n as usize);
        assert_eq!(raw.len(), h * (1 + w * 3));

        for y in 0..h {
            let line = &raw[y * (1 + w * 3)..(y + 1) * (1 + w * 3)];
            assert_eq!(line[0], 0, "row {} is not filter None", y);
            for x in 0..w {
                let src = y * stride + x * 4;
                assert_eq!(
                    &line[1 + x * 3..4 + x * 3],
                    &fb[src + 1..src + 4],
                    "pixel ({}, {}) came back wrong",
                    x,
                    y
                );
            }
        }
    }

    /// The packing on its own, since it is what the C shim on the target has to
    /// reproduce and the only thing that stands between a real frame and a PNG
    /// with red and blue swapped. `--probe-display` runs the same comparison on
    /// the machine, where the two sides are genuinely different code.
    #[test]
    fn packing_drops_the_alpha_and_keeps_the_channel_order() {
        let (w, h, stride) = (3, 2, 24);
        let fb = fixture(w, h, stride);
        let packed = pack_rgb_rows_rust(&fb, stride, w, h);
        assert_eq!(packed.len(), h * (1 + w * 3));
        for y in 0..h {
            let line = &packed[y * (1 + w * 3)..(y + 1) * (1 + w * 3)];
            assert_eq!(line[0], 0, "row {} lost its filter byte", y);
            for x in 0..w {
                let p = y * stride + x * 4;
                assert_eq!(line[1 + x * 3], fb[p + 1], "R at ({}, {})", x, y);
                assert_eq!(line[2 + x * 3], fb[p + 2], "G at ({}, {})", x, y);
                assert_eq!(line[3 + x * 3], fb[p + 3], "B at ({}, {})", x, y);
            }
        }
        // The alpha byte is 0xff throughout the fixture, so a packing that let it
        // through would put a 0xff in every fourth output byte.
        assert!(packed.iter().any(|b| *b != 0xff), "output looks like alpha");
        assert_eq!(pack_rgb_rows(&fb, stride, w, h), packed);
    }

    #[test]
    fn a_geometry_that_does_not_fit_is_refused_rather_than_read_past() {
        assert!(encode_argb(&[0; 64], 32, 0, 3).is_err());
        assert!(encode_argb(&[0; 64], 32, 4, 0).is_err());
        // stride narrower than the pixels it claims to hold
        assert!(encode_argb(&[0; 64], 8, 4, 3).is_err());
        // a buffer one byte short of its own geometry
        assert!(encode_argb(&[0; 95], 32, 4, 3).is_err());
        assert!(encode_argb(&[0; 96], 32, 4, 3).is_ok());
    }
}
