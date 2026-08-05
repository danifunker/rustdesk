//! Write PNGs from `png::encode_argb` to disk so a real decoder can verify them.
//! The unit tests check the structure against zlib's own inflate; this checks the
//! claim that matters -- that an actual image library accepts the file and gets
//! the pixels we put in. A spec-legal file that no decoder reads would pass every
//! test in the module.
//!
//! ```text
//! cargo run --example png_check -- /tmp/out
//! python3 -c "..."   # see docs/BACKLOG.md, or the assertion below
//! ```
//!
//! Each PNG is written beside a `.raw` of the RGB triples it should decode to, so
//! the comparison needs no knowledge of our encoder at all.

/// A gradient with a different slope in every channel, so a swap shows up.
fn fixture(width: usize, height: usize, stride: usize) -> Vec<u8> {
    let mut fb = vec![0u8; stride * height];
    for y in 0..height {
        for x in 0..width {
            let p = y * stride + x * 4;
            fb[p] = 0xff;
            fb[p + 1] = (x * 7 + y) as u8;
            fb[p + 2] = (x + y * 3) as u8;
            fb[p + 3] = (x * 2 + y * 5) as u8;
        }
    }
    fb
}

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| ".".into());
    // The last is the real geometry, with the padded stride a 1920-wide
    // framebuffer actually has.
    for (name, w, h, stride) in [
        ("tiny", 1usize, 1usize, 4usize),
        ("odd", 9, 5, 48),
        ("square", 64, 64, 256),
        ("display", 1920, 1080, 7680),
    ] {
        let fb = fixture(w, h, stride);
        let png = rustdesk_ppc_agent::png::encode_argb(&fb, stride, w, h).unwrap();

        // What a decoder should hand back: the R, G, B of every pixel, in order,
        // with no stride padding and no alpha.
        let mut want = Vec::with_capacity(w * h * 3);
        for y in 0..h {
            for x in 0..w {
                let p = y * stride + x * 4;
                want.extend_from_slice(&fb[p + 1..p + 4]);
            }
        }

        std::fs::write(format!("{}/{}.png", dir, name), &png).unwrap();
        std::fs::write(format!("{}/{}.raw", dir, name), &want).unwrap();
        println!(
            "{}: {}x{} -> {} bytes ({:.1}% of raw)",
            name,
            w,
            h,
            png.len(),
            100.0 * png.len() as f64 / want.len() as f64
        );
    }
}
