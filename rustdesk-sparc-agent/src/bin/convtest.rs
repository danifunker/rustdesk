//! convtest -- is `rd_argb_to_i420_rect` the same conversion the rest of the
//! agent uses?
//!
//! The fused downscale-and-convert in `capture_shim.c` is the frame loop's hot
//! path, and it is the one piece of this port that was written by adapting the
//! IRIX port's rather than by porting the shared Rust. The IRIX canvas is
//! A,B,G,R and this one is A,R,G,B, so every source offset in it had to move.
//! Getting that wrong swaps red and blue in every frame -- which, seen over a
//! remote display, looks like a fault in the client.
//!
//! So this checks it against `convert::argb_to_i420`, which is a different
//! implementation in a different tree that `prototest` has already proved on
//! this machine, on synthetic images whose right answer is known. No display,
//! no encoder, no protocol: a failure here is the conversion's.
//!
//! What "the same" means, exactly. The luma is bit-identical, and is checked
//! that way. The chroma is not, and cannot be: the shared converter averages
//! each channel and then weights it, while this one weights the sums and
//! shifts once, which keeps the fractional part of the average through the
//! weighting. That is a difference of at most one code, and this measures it
//! rather than asserting it -- a red/blue swap moves U and V by ~150.

use rustdesk_ppc_agent::convert::{argb_to_i420, I420};

use std::os::raw::{c_int, c_uchar};

extern "C" {
    fn rd_argb_to_i420_rect(
        src: *const c_uchar,
        src_len: usize,
        src_stride: c_int,
        yp: *mut c_uchar,
        up: *mut c_uchar,
        vp: *mut c_uchar,
        dst_w: c_int,
        dst_h: c_int,
        chroma_stride: c_int,
        factor: c_int,
        dx0: c_int,
        dy0: c_int,
        dx1: c_int,
        dy1: c_int,
    ) -> c_int;
}

/// The shim, over the whole destination.
fn shim(src: &[u8], stride: usize, dst: &mut I420, factor: i32) -> i32 {
    let (w, h) = (dst.width as c_int, dst.height as c_int);
    let cs = dst.chroma_stride() as c_int;
    unsafe {
        rd_argb_to_i420_rect(
            src.as_ptr(), src.len(), stride as c_int,
            dst.y.as_mut_ptr(), dst.u.as_mut_ptr(), dst.v.as_mut_ptr(),
            w, h, cs, factor as c_int,
            0, 0, w, h,
        )
    }
}

/// The shim, over one destination rectangle.
fn shim_rect(
    src: &[u8], stride: usize, dst: &mut I420, factor: i32,
    dx0: i32, dy0: i32, dx1: i32, dy1: i32,
) -> i32 {
    let (w, h) = (dst.width as c_int, dst.height as c_int);
    let cs = dst.chroma_stride() as c_int;
    unsafe {
        rd_argb_to_i420_rect(
            src.as_ptr(), src.len(), stride as c_int,
            dst.y.as_mut_ptr(), dst.u.as_mut_ptr(), dst.v.as_mut_ptr(),
            w, h, cs, factor as c_int,
            dx0 as c_int, dy0 as c_int, dx1 as c_int, dy1 as c_int,
        )
    }
}

/// An A,R,G,B framebuffer: byte 0 alpha, byte 1 red, byte 3 blue.
fn fb<F: Fn(usize, usize) -> (u8, u8, u8)>(w: usize, h: usize, f: F) -> Vec<u8> {
    let mut v = vec![0u8; w * h * 4];
    for y in 0..h {
        for x in 0..w {
            let (r, g, b) = f(x, y);
            let o = (y * w + x) * 4;
            v[o] = 0xff;
            v[o + 1] = r;
            v[o + 2] = g;
            v[o + 3] = b;
        }
    }
    v
}

/// Largest absolute difference between two planes, and how many samples differ.
fn diff(a: &[u8], b: &[u8]) -> (i32, usize) {
    let mut max = 0i32;
    let mut n = 0usize;
    for i in 0..a.len().min(b.len()) {
        let d = (a[i] as i32 - b[i] as i32).abs();
        if d != 0 {
            n += 1;
        }
        if d > max {
            max = d;
        }
    }
    (max, n)
}

fn main() {
    let mut failures = 0;

    // --- 1. factor 1, against the shared converter -------------------------
    //
    // A pattern with every channel moving independently, so a channel that is
    // read from the wrong byte cannot coincidentally agree.
    {
        let (w, h) = (64usize, 32usize);
        let src = fb(w, h, |x, y| {
            ((x * 4) as u8, (y * 8) as u8, (x * 2 + y * 3) as u8)
        });
        let mut want = I420::new(w, h);
        argb_to_i420(&src, w * 4, &mut want);
        let mut got = I420::new(w, h);
        let rc = shim(&src, w * 4, &mut got, 1);

        let (ymax, yn) = diff(&want.y, &got.y);
        let (umax, un) = diff(&want.u, &got.u);
        let (vmax, vn) = diff(&want.v, &got.v);
        println!(
            "factor 1 vs convert::argb_to_i420 (rc {}): Y max {} ({} of {}), \
             U max {} ({}), V max {} ({})",
            rc, ymax, yn, want.y.len(), umax, un, vmax, vn
        );
        if rc != 0 {
            println!("  FAIL: refused a 64x32 factor-1 conversion");
            failures += 1;
        }
        if ymax != 0 {
            println!("  FAIL: luma is supposed to be bit-identical");
            failures += 1;
        }
        if umax > 1 || vmax > 1 {
            println!("  FAIL: chroma should differ by at most one code; a swap shows here");
            failures += 1;
        }
    }

    // --- 2. red and blue, named -------------------------------------------
    //
    // The failure this whole file exists for. Solid red and solid blue are the
    // two colours that a swapped pair of source offsets turns into each other,
    // and BT.601 puts them far apart: red is V 240 / U 90, blue is U 240 /
    // V 110. Anything that reads byte 3 as red reports these exactly the wrong
    // way round.
    {
        let cases: [(&str, (u8, u8, u8), u8, u8, u8); 4] = [
            ("red",   (255, 0, 0),     82, 90,  240),
            ("blue",  (0, 0, 255),     41, 240, 110),
            ("green", (0, 255, 0),    144, 54,  34),
            ("white", (255, 255, 255), 235, 128, 128),
        ];
        for &(name, (r, g, b), ey, eu, ev) in cases.iter() {
            let (w, h) = (16usize, 16usize);
            let src = fb(w, h, |_, _| (r, g, b));
            let mut out = I420::new(w, h);
            let rc = shim(&src, w * 4, &mut out, 1);
            let (gy, gu, gv) = (out.y[0], out.u[0], out.v[0]);
            let ok = rc == 0 && gy == ey && gu == eu && gv == ev;
            println!(
                "{:<5} rgb({:>3},{:>3},{:>3}) -> Y={:>3} U={:>3} V={:>3}   expect Y={} U={} V={}  {}",
                name, r, g, b, gy, gu, gv, ey, eu, ev, if ok { "ok" } else { "FAIL" }
            );
            if !ok {
                failures += 1;
                if name == "red" && gu == 240 {
                    println!("  red came out with blue's chroma: the source offsets are swapped");
                }
            }
        }
    }

    // --- 3. the downscale, on images whose average is known ----------------
    //
    // Every source pixel inside one destination pixel's box is given the same
    // colour, so the box average is that colour whatever the kernel does with
    // it -- which means the right answer is the shared converter's answer on
    // the small image, and all three kernels (1, 2, and the sampled one above
    // 2) have to agree with it. That is what makes this a check of the
    // addressing rather than a restatement of the arithmetic.
    for &factor in [1i32, 2, 4, 8].iter() {
        let (dw, dh) = (16usize, 16usize);
        let f = factor as usize;
        let (w, h) = (dw * f, dh * f);
        let colour = |dx: usize, dy: usize| {
            ((dx * 16) as u8, (dy * 16) as u8, ((dx + dy) * 8) as u8)
        };
        let src = fb(w, h, |x, y| colour(x / f, y / f));
        let small = fb(dw, dh, |x, y| colour(x, y));

        let mut want = I420::new(dw, dh);
        argb_to_i420(&small, dw * 4, &mut want);
        let mut got = I420::new(dw, dh);
        let rc = shim(&src, w * 4, &mut got, factor);

        let (ymax, _) = diff(&want.y, &got.y);
        let (umax, _) = diff(&want.u, &got.u);
        let (vmax, _) = diff(&want.v, &got.v);
        let ok = rc == 0 && ymax == 0 && umax <= 1 && vmax <= 1;
        println!(
            "factor {} on constant boxes: {}x{} -> {}x{} (rc {}) Y max {} U max {} V max {}  {}",
            factor, w, h, dw, dh, rc, ymax, umax, vmax, if ok { "ok" } else { "FAIL" }
        );
        if !ok {
            failures += 1;
        }
    }

    // --- 4. the box really is averaged at 1/2 ------------------------------
    //
    // A per-pixel checkerboard: point-sampling gives 235 or 16, averaging
    // gives 126. Only factors 1 and 2 are checked, because above 1/2 the
    // kernel deliberately samples a 2x2 of each box instead of averaging all
    // of it -- and on a checkerboard those four samples land on one colour, so
    // the documented behaviour would read here as a failure.
    {
        let (dw, dh) = (16usize, 16usize);
        let (w, h) = (dw * 2, dh * 2);
        let src = fb(w, h, |x, y| {
            if (x + y) % 2 == 0 { (255, 255, 255) } else { (0, 0, 0) }
        });
        let mut got = I420::new(dw, dh);
        let rc = shim(&src, w * 4, &mut got, 2);
        let y = got.y[dw + 1];
        let ok = rc == 0 && (y as i32 - 126).abs() <= 2 && got.u[1] == 128 && got.v[1] == 128;
        println!(
            "1/2 of a checkerboard: Y={} U={} V={} (expect ~126/128/128, not 235 or 16)  {}",
            y, got.u[1], got.v[1], if ok { "ok" } else { "FAIL" }
        );
        if !ok {
            failures += 1;
        }
    }

    // --- 5. a rectangle writes only its rectangle --------------------------
    //
    // The whole reason the function takes destination bounds: a frame should
    // cost what the damage costs. If it wrote outside them the saving would be
    // real and the picture would be wrong in a way that only shows when two
    // regions change in different frames.
    {
        let (w, h) = (64usize, 64usize);
        let src = fb(w, h, |x, y| ((x * 3) as u8, (y * 3) as u8, 200));
        let mut out = I420::new(w, h);
        for b in out.y.iter_mut() { *b = 7; }
        for b in out.u.iter_mut() { *b = 7; }
        for b in out.v.iter_mut() { *b = 7; }
        let rc = shim_rect(&src, w * 4, &mut out, 1, 8, 8, 24, 24);

        let mut inside_untouched = 0;
        let mut outside_touched = 0;
        for y in 0..h {
            for x in 0..w {
                let v = out.y[y * w + x];
                let inside = x >= 8 && x < 24 && y >= 8 && y < 24;
                if inside && v == 7 { inside_untouched += 1; }
                if !inside && v != 7 { outside_touched += 1; }
            }
        }
        let ok = rc == 0 && inside_untouched == 0 && outside_touched == 0;
        println!(
            "rect 8,8..24,24 of 64x64 (rc {}): {} inside left unwritten, {} outside overwritten  {}",
            rc, inside_untouched, outside_touched, if ok { "ok" } else { "FAIL" }
        );
        if !ok {
            failures += 1;
        }

        // ...and what it did write is what the whole-frame conversion writes.
        let mut whole = I420::new(w, h);
        shim(&src, w * 4, &mut whole, 1);
        let mut disagree = 0;
        for y in 8..24 {
            for x in 8..24 {
                if out.y[y * w + x] != whole.y[y * w + x] { disagree += 1; }
            }
        }
        println!(
            "  the rectangle's pixels against the whole-frame conversion: {} disagree  {}",
            disagree, if disagree == 0 { "ok" } else { "FAIL" }
        );
        if disagree != 0 {
            failures += 1;
        }
    }

    // --- 6. odd bounds snap outward, they do not clip ----------------------
    //
    // A 4:2:0 chroma sample is shared by a 2x2 destination block, so a bound
    // falling inside one would leave half of it unwritten. The contract is to
    // snap outward; this checks the whole block gets written.
    {
        let (w, h) = (32usize, 32usize);
        let src = fb(w, h, |x, y| ((x * 8) as u8, (y * 8) as u8, 64));
        let mut out = I420::new(w, h);
        for b in out.y.iter_mut() { *b = 7; }
        let rc = shim_rect(&src, w * 4, &mut out, 1, 5, 5, 11, 11);
        // 5..11 snaps to 4..12.
        let mut holes = 0;
        for y in 4..12 { for x in 4..12 { if out.y[y * w + x] == 7 { holes += 1; } } }
        let mut spill = 0;
        for y in 0..h { for x in 0..w {
            let inside = x >= 4 && x < 12 && y >= 4 && y < 12;
            if !inside && out.y[y * w + x] != 7 { spill += 1; }
        } }
        let ok = rc == 0 && holes == 0 && spill == 0;
        println!(
            "odd bounds 5,5..11,11 snap to 4,4..12,12 (rc {}): {} holes, {} spill  {}",
            rc, holes, spill, if ok { "ok" } else { "FAIL" }
        );
        if !ok {
            failures += 1;
        }
    }

    // --- 7. what it refuses ------------------------------------------------
    //
    // Refusing is the contract, not a crash and not a quiet rounding: the
    // session picks the scale from the peer's requested quality, and a
    // non-power-of-two arriving there must convert nothing rather than
    // convert wrongly.
    {
        let (w, h) = (32usize, 32usize);
        let src = fb(w, h, |_, _| (10, 20, 30));
        let mut out = I420::new(w / 2, h / 2);
        let mut bad = 0;
        for &(name, factor) in [("3", 3i32), ("0", 0), ("-1", -1), ("6", 6), ("16", 16)].iter() {
            let rc = shim(&src, w * 4, &mut out, factor);
            println!("  factor {:>2} -> rc {} {}", name, rc, if rc == -1 { "(refused, ok)" } else { "FAIL" });
            if rc != -1 { bad += 1; }
        }
        // A source too small for the destination it was asked for.
        let mut big = I420::new(w * 2, h * 2);
        let rc = shim(&src, w * 4, &mut big, 1);
        println!("  destination larger than the source -> rc {} {}", rc,
                 if rc == -1 { "(refused, ok)" } else { "FAIL" });
        if rc != -1 { bad += 1; }
        // An empty rectangle is not an error; it is nothing to do.
        let mut out2 = I420::new(w, h);
        let rc = shim_rect(&src, w * 4, &mut out2, 1, 10, 10, 10, 10);
        println!("  an empty rectangle -> rc {} {}", rc,
                 if rc == 0 { "(nothing to do, ok)" } else { "FAIL" });
        if rc != 0 { bad += 1; }
        failures += bad;
    }

    println!();
    if failures == 0 {
        println!("convtest: all checks passed");
    } else {
        println!("convtest: {} check(s) FAILED", failures);
        std::process::exit(1);
    }
}
