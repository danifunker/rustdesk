//! What a frame costs when almost nothing changed -- the case that decides how
//! the pointer feels.
//!
//! A moving pointer dirties a handful of macroblocks. With an active map the
//! encoder should spend nearly nothing on the rest, and yet on the O2 a frame
//! with 4-16 active macroblocks took a median 194 ms at 1280x1024 and 55 ms at
//! 640x512 in a live session. That floor is the pointer's frame rate and its
//! latency, so this measures it in isolation: no X, no capture, no network.
//!
//!     encfloor [width height [frames [profile [screen_content [size]]]]]
//!     encfloor verify [width height [frames]]
//!
//! A key frame of a synthetic desktop, then `frames` inter frames in each of
//! which only a `size`-pixel square (24, a pointer) moves, with the active map
//! covering its old and new positions -- what the session does for a pointer
//! move, or with a larger size, for a window being dragged. Prints the
//! median, minimum and maximum per frame. Run it under tools/pcsample.c to see
//! where inside libvpx the floor is spent.
//!
//! `verify` is for changes to libvpx that are meant to make it faster and
//! nothing else: a long, deliberately awkward sequence -- the pointer, bigger
//! regions, frames with an empty map, frames with no map at all, forced key
//! frames -- with a hash of every encoded byte. Two libvpx builds that print
//! the same hash wrote the same bitstream.

use rustdesk_ppc_agent::convert::I420;
use rustdesk_ppc_agent::encode::{Encoder, Tune};
use std::time::Instant;

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.first().map(|s| s.as_str()) == Some("verify") {
        return verify(&a[1..]);
    }
    let num = |i: usize, d: usize| a.get(i).and_then(|s| s.parse().ok()).unwrap_or(d);
    let (w, h) = (num(0, 1280), num(1, 1024));
    let frames = num(2, 60);
    let tune = Tune {
        profile: num(3, 3) as u32,
        screen_content: num(4, 2) as u32,
        ..Tune::default()
    };

    let mut img = I420::new(w, h);
    // Something like a desktop: flat areas with some edges, so the key frame is
    // not trivially cheap and the reference is not all one value.
    for y in 0..h {
        for x in 0..w {
            img.y[y * w + x] = if (x / 64 + y / 48) % 3 == 0 { 200 } else { ((x ^ y) & 0x3f) as u8 + 60 };
        }
    }
    for v in img.u.iter_mut().chain(img.v.iter_mut()) {
        *v = 128;
    }

    let mut enc = Encoder::tuned(w, h, 1500, tune).expect("encoder");
    let t = Instant::now();
    let key = enc.encode(&img, 0, true).expect("key frame").data.len();
    println!("{}x{} {:?}", w, h, tune);
    println!("key frame: {} ms, {} bytes", t.elapsed().as_millis(), key);

    let sq = num(5, 24).clamp(8, w.min(h) / 2);
    let (mut px, mut py) = (w / 3, h / 3);
    let mut times = Vec::with_capacity(frames);
    let mut bytes = 0usize;
    let mut active = 0usize;
    for f in 1..=frames {
        let (ox, oy) = (px, py);
        px = (px + 7) % (w - sq);
        py = (py + 3) % (h - sq);
        // Erase the old square, draw the new one.
        for y in 0..sq {
            for x in 0..sq {
                img.y[(oy + y) * w + ox + x] = 200;
            }
        }
        for y in 0..sq {
            for x in 0..sq {
                img.y[(py + y) * w + px + x] = 16;
            }
        }
        enc.active_none();
        enc.active_rect(ox as i32, oy as i32, sq as i32, sq as i32);
        enc.active_rect(px as i32, py as i32, sq as i32, sq as i32);
        active += enc.active_count();
        let t = Instant::now();
        bytes += enc.encode(&img, f as i64 * 40, false).expect("inter frame").data.len();
        times.push(t.elapsed().as_micros() as u64);
    }
    times.sort_unstable();
    let ms = |us: u64| us as f64 / 1000.0;
    println!(
        "{} inter frames, {:.1} active MB each: median {:.1} ms, min {:.1}, max {:.1}; {} bytes each",
        frames,
        active as f64 / frames as f64,
        ms(times[times.len() / 2]),
        ms(times[0]),
        ms(times[times.len() - 1]),
        bytes / frames.max(1)
    );
}

/// FNV-1a over every encoded byte, and a per-frame line for the first frame
/// where two builds part company.
fn verify(a: &[String]) {
    let num = |i: usize, d: usize| a.get(i).and_then(|s| s.parse().ok()).unwrap_or(d);
    let (w, h) = (num(0, 640), num(1, 512));
    let frames = num(2, 400);
    let tune = Tune { profile: 3, screen_content: 2, ..Tune::default() };
    let mut enc = Encoder::tuned(w, h, 1500, tune).expect("encoder");
    let mut img = I420::new(w, h);
    let mut seed: u32 = 12345;
    let mut rnd = move |n: usize| {
        seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
        ((seed >> 8) as usize) % n.max(1)
    };
    for (i, v) in img.y.iter_mut().enumerate() {
        *v = ((i * 7) % 251) as u8;
    }
    for v in img.u.iter_mut().chain(img.v.iter_mut()) {
        *v = 128;
    }
    let mut total: u32 = 0x811c9dc5;
    let mut bytes = 0usize;
    let t = Instant::now();
    for f in 0..frames {
        // What changes this frame, and what the map says about it.
        let kind = if f == 0 { 0 } else { rnd(20) };
        let mut rects: Vec<(usize, usize, usize, usize)> = Vec::new();
        match kind {
            0 => {}                                       // key frame / map off
            1 => {}                                       // map, nothing in it
            2 | 3 => rects.push((rnd(w - 200), rnd(h - 120), 200, 120)), // a window
            4 => rects.push((0, rnd(h - 16), w, 16)),     // a full-width band
            _ => {                                        // the pointer, or two
                for _ in 0..1 + rnd(2) {
                    rects.push((rnd(w - 24), rnd(h - 24), 24, 24));
                }
            }
        }
        for &(x0, y0, rw, rh) in &rects {
            let c = rnd(256) as u8;
            for y in y0..y0 + rh {
                for x in x0..x0 + rw {
                    img.y[y * w + x] = c ^ ((x + y) as u8 & 0x1f);
                }
            }
            let cw = w / 2;
            for y in y0 / 2..(y0 + rh) / 2 {
                for x in x0 / 2..(x0 + rw) / 2 {
                    img.u[y * cw + x] = c;
                    img.v[y * cw + x] = 255 - c;
                }
            }
        }
        let force_key = f == 0 || (kind == 0 && rnd(3) == 0);
        if kind == 0 {
            enc.active_all();
        } else {
            enc.active_none();
            for &(x0, y0, rw, rh) in &rects {
                enc.active_rect(x0 as i32, y0 as i32, rw as i32, rh as i32);
            }
        }
        let out = enc.encode(&img, f as i64 * 40, force_key).expect("encode");
        let mut hsh: u32 = 0x811c9dc5;
        for &b in out.data {
            hsh = (hsh ^ b as u32).wrapping_mul(16777619);
            total = (total ^ b as u32).wrapping_mul(16777619);
        }
        bytes += out.data.len();
        if a.iter().any(|s| s == "frames") {
            println!("frame {:4} kind {:2} key {} {:6} bytes {:08x}", f, kind, out.key as u8, out.data.len(), hsh);
        }
    }
    println!(
        "verify {}x{}: {} frames, {} bytes, stream hash {:08x}, {} ms",
        w,
        h,
        frames,
        bytes,
        total,
        t.elapsed().as_millis()
    );
}
