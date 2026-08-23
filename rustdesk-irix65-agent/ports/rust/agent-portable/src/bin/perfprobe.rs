//! Where a frame's milliseconds actually go, one stage and one frame at a time.
//!
//! `pipeline` averages two rounds per resolution, one of which is a forced
//! keyframe. That single decision hid the largest fact about this encoder:
//! **a keyframe and an inter frame do not cost remotely the same**, and the
//! live agent was sending nothing but keyframes. An average of the two is a
//! number that describes neither.
//!
//! So this reports per frame, and it measures the three things the frame loop
//! can actually change:
//!
//!   1. keyframe against inter, at each scale
//!   2. the fused scale+convert against scale-then-convert
//!   3. an inter frame with an active map covering only the damage, against one
//!      that lets the encoder look at the whole screen
//!
//! It also dumps raw canvas bytes, because every conversion below depends on
//! the memory order being A,B,G,R and that has only ever been measured once.

#[cfg(target_os = "irix")]
fn main() {
    use rustdesk_ppc_agent::capture::Capturer;
    use rustdesk_ppc_agent::convert::{argb_to_i420_rows, I420};
    use rustdesk_ppc_agent::encode::Encoder;
    use std::time::Instant;

    fn ms(t: Instant) -> f64 {
        let d = t.elapsed();
        d.as_secs() as f64 * 1000.0 + d.subsec_nanos() as f64 / 1.0e6
    }

    let args: Vec<String> = std::env::args().skip(1).collect();
    let want = |name: &str| args.iter().any(|a| a == name);
    let all = args.is_empty();

    let mut cap = match Capturer::new() {
        Ok(c) => c,
        Err(e) => {
            println!("capture unavailable: {}", e);
            return;
        }
    };
    println!(
        "capture: {} on {}x{}, screen depth {}, cursor embedded {}",
        cap.path_name(), cap.width, cap.height, cap.screen_depth(), cap.cursor_embedded()
    );

    let t = Instant::now();
    cap.frame();
    println!("initial full read: {:.0} ms", ms(t));

    // ---------------------------------------------------------------- bytes
    // The byte order everything else assumes. Run `xsetroot -solid red` first
    // and the answer is unambiguous: A,B,G,R puts ff in byte 3, A,R,G,B in
    // byte 1.
    if all || want("bytes") {
        let stride = cap.stride();
        let buf = cap.buffer();
        print!("\ncanvas bytes, row 0:  ");
        for i in 0..8 {
            print!("{:02x} ", buf[i]);
        }
        print!("\ncanvas bytes, centre: ");
        let mid = (cap.height / 2) * stride + (cap.width / 2) * 4;
        for i in 0..8 {
            print!("{:02x} ", buf[mid + i]);
        }
        // Count how many pixels have a non-zero byte 1 versus byte 3, which
        // separates a red screen from a blue one without needing to know which
        // way round the answer is.
        let (mut b1, mut b3) = (0usize, 0usize);
        let mut y = 0;
        while y < cap.height {
            let row = y * stride;
            let mut x = 0;
            while x < cap.width {
                if buf[row + x * 4 + 1] > 32 { b1 += 1; }
                if buf[row + x * 4 + 3] > 32 { b3 += 1; }
                x += 16;
            }
            y += 16;
        }
        println!("\nsampled pixels with byte1 lit: {}, byte3 lit: {}", b1, b3);
        println!("  (A,B,G,R means byte1 is blue and byte3 is red)");
    }

    // ----------------------------------------------------------- conversion
    if all || want("convert") {
        println!("\n=== conversion: two passes against one ===");
        for &factor in &[4i32, 2, 1] {
            let (w, h) = ((cap.width / factor as usize) & !1, (cap.height / factor as usize) & !1);
            let mut a = I420::new(w, h);
            let mut b = I420::new(w, h);

            let native_stride = cap.stride();
            let t = Instant::now();
            let (src, sw, _sh) = cap.scaled(factor);
            let sc = ms(t);
            let stride = if factor == 1 { native_stride } else { sw * 4 };
            let t = Instant::now();
            argb_to_i420_rows(src, stride, &mut a, 0, h);
            let cv = ms(t);

            let t = Instant::now();
            let ok = cap.to_i420(&mut b, factor);
            let fused = ms(t);

            println!(
                "  1/{} -> {}x{}:  scale {:.0} + convert {:.0} = {:.0} ms   fused {:.0} ms  ({:.1}x)",
                factor, w, h, sc, cv, sc + cv, fused, (sc + cv) / fused.max(0.001)
            );
            if !ok {
                println!("    fused refused this shape");
            }
            // How far apart the two answers are. They will not match: the old
            // path reads A,R,G,B and this framebuffer is A,B,G,R, so the
            // difference *is* the red/blue swap. Reported as a number so it is
            // not a matter of opinion.
            let n = a.y.len().min(b.y.len());
            let mut ydiff = 0usize;
            let mut udiff = 0usize;
            for i in 0..n {
                if a.y[i] != b.y[i] { ydiff += 1; }
            }
            for i in 0..a.u.len().min(b.u.len()) {
                if a.u[i] != b.u[i] { udiff += 1; }
            }
            println!("    planes differ: Y {}/{}, U {}/{}", ydiff, n, udiff, a.u.len());
        }
    }

    // -------------------------------------------------------------- encoder
    if all || want("encode") {
        println!("\n=== encode: keyframe against inter, per frame ===");
        for &factor in &[4i32, 2, 1] {
            let (w, h) = ((cap.width / factor as usize) & !1, (cap.height / factor as usize) & !1);
            let kbps = (1000 / (factor * factor)).max(120) as u32;
            let mut enc = match Encoder::new(w, h, kbps) {
                Ok(e) => e,
                Err(e) => { println!("  encoder refused {}x{}: {}", w, h, e); continue; }
            };
            let mut img = I420::new(w, h);
            cap.to_i420(&mut img, factor);
            println!("  1/{} -> {}x{}, {} macroblocks", factor, w, h, enc.mb_rows() * enc.mb_cols());

            for round in 0..4 {
                let force = round == 0;
                let t = Instant::now();
                let (n, key) = match enc.encode(&img, round as i64 * 100, force) {
                    Ok(f) => (f.data.len(), f.key),
                    Err(e) => { println!("    encode failed: {}", e); (0, false) }
                };
                println!(
                    "    frame {} {}: {:7.0} ms, {} bytes",
                    round, if key { "KEY  " } else { "inter" }, ms(t), n
                );
            }
        }
    }

    // ------------------------------------------------------------ activemap
    if all || want("amap") {
        println!("\n=== active map: encode only what changed ===");
        for &factor in &[4i32, 2, 1] {
            let (w, h) = ((cap.width / factor as usize) & !1, (cap.height / factor as usize) & !1);
            let kbps = (1000 / (factor * factor)).max(120) as u32;
            let mut enc = match Encoder::new(w, h, kbps) {
                Ok(e) => e,
                Err(e) => { println!("  encoder refused {}x{}: {}", w, h, e); continue; }
            };
            let total = enc.mb_rows() * enc.mb_cols();
            let mut img = I420::new(w, h);
            cap.to_i420(&mut img, factor);
            let _ = enc.encode(&img, 0, true);          // the keyframe, discarded
            let _ = enc.encode(&img, 100, false);       // settle

            // Whole frame considered.
            let t = Instant::now();
            let n = enc.encode(&img, 200, false).map(|f| f.data.len()).unwrap_or(0);
            let full = ms(t);

            // A 160x128 patch, which is about what a line of text or a small
            // window redraw actually dirties.
            enc.active_none();
            enc.active_rect(0, 0, 160.min(w as i32), 128.min(h as i32));
            let act = enc.active_count();
            let t = Instant::now();
            let n2 = enc.encode(&img, 300, false).map(|f| f.data.len()).unwrap_or(0);
            let small = ms(t);
            enc.active_all();

            println!(
                "  1/{} -> {}x{}: whole frame {:.0} ms ({} B, {} MBs)   patch {:.0} ms ({} B, {} MBs)   {:.1}x",
                factor, w, h, full, n, total, small, n2, act, full / small.max(0.001)
            );
        }
    }

    // ----------------------------------------------------------------- dump
    // The colour check that has never been done. Convert the screen exactly as
    // the video path does, take the I420 back to RGB the way a decoder would,
    // and write a PPM. Anyone can then look at it: red and blue swapped is
    // trivial to see and, in a still of a grey desktop, almost impossible to
    // reason about.
    if all || want("dump") {
        let factor = 2i32;
        let (w, h) = ((cap.width / factor as usize) & !1, (cap.height / factor as usize) & !1);
        let mut img = I420::new(w, h);
        cap.to_i420(&mut img, factor);
        let cs = img.chroma_stride();
        let mut ppm = format!("P6\n{} {}\n255\n", w, h).into_bytes();
        for y in 0..h {
            for x in 0..w {
                // BT.601 studio swing, the inverse of what the converter did.
                let yy = img.y[y * w + x] as i32 - 16;
                let u = img.u[(y / 2) * cs + x / 2] as i32 - 128;
                let v = img.v[(y / 2) * cs + x / 2] as i32 - 128;
                let c = |n: i32| n.clamp(0, 255) as u8;
                ppm.push(c((298 * yy + 409 * v + 128) >> 8));
                ppm.push(c((298 * yy - 100 * u - 208 * v + 128) >> 8));
                ppm.push(c((298 * yy + 516 * u + 128) >> 8));
            }
        }
        match std::fs::write("/tmp/frame.ppm", &ppm) {
            Ok(_) => println!("\nwrote /tmp/frame.ppm, {}x{}, {} bytes", w, h, ppm.len()),
            Err(e) => println!("\ncould not write /tmp/frame.ppm: {}", e),
        }
    }

    // --------------------------------------------------------------- verify
    // Does the peer's picture still match the screen?
    //
    // This is the check the whole damage-driven design rests on, and nothing
    // else covers it. An inactive macroblock keeps the previous frame's pixels
    // *for ever*, so a damage report that misses a change -- or an encoder
    // patch that skips one it should have coded -- does not produce a slightly
    // late picture, it produces a permanently wrong one, in a rectangle, with
    // no error anywhere and no way for the peer to know.
    //
    // So: take the frame `testpeer` decoded, capture the same screen now,
    // convert it the same way, and report how far apart they are. A block that
    // rotted shows up as a cluster of large differences; ordinary encoder loss
    // shows up as a small mean and nothing else.
    if want("verify") {
        let decoded = match std::fs::read("/tmp/decoded.ppm") {
            Ok(d) => d,
            Err(e) => {
                println!("\nno /tmp/decoded.ppm ({}); run testpeer first", e);
                return;
            }
        };
        // "P6\n<w> <h>\n255\n"
        let header_end = decoded
            .windows(4)
            .position(|w| w == b"255\n")
            .map(|i| i + 4)
            .unwrap_or(0);
        let hdr = String::from_utf8_lossy(&decoded[..header_end]).to_string();
        let nums: Vec<usize> = hdr
            .split_whitespace()
            .filter_map(|t| t.parse().ok())
            .collect();
        if header_end == 0 || nums.len() < 3 {
            println!("\n/tmp/decoded.ppm is not a PPM this can read");
            return;
        }
        let (dw, dh) = (nums[0], nums[1]);
        let factor = (cap.width / dw.max(1)) as i32;
        println!("\n=== the peer's picture against the screen ===");
        println!("  decoded {}x{}, which is 1/{} of {}x{}", dw, dh, factor, cap.width, cap.height);

        let mut img = I420::new(dw, dh);
        if !cap.to_i420(&mut img, factor) {
            println!("  could not re-convert at 1/{}", factor);
            return;
        }
        let cs = img.chroma_stride();
        let mut total = 0u64;
        let mut over = 0usize;
        // Worst 16x16 block, which is a macroblock: that is the unit the active
        // map works in, so a fault in it lands inside exactly one of these.
        let (mut worst, mut worst_at) = (0u32, (0usize, 0usize));
        let mut blocks = vec![0u32; ((dw + 15) / 16) * ((dh + 15) / 16)];
        let bcols = (dw + 15) / 16;
        for y in 0..dh {
            for x in 0..dw {
                let yy = img.y[y * dw + x] as i32 - 16;
                let u = img.u[(y / 2) * cs + x / 2] as i32 - 128;
                let v = img.v[(y / 2) * cs + x / 2] as i32 - 128;
                let c = |n: i32| n.clamp(0, 255);
                let fresh = [
                    c((298 * yy + 409 * v + 128) >> 8),
                    c((298 * yy - 100 * u - 208 * v + 128) >> 8),
                    c((298 * yy + 516 * u + 128) >> 8),
                ];
                let o = header_end + (y * dw + x) * 3;
                if o + 2 >= decoded.len() {
                    continue;
                }
                let mut d = 0i32;
                for k in 0..3 {
                    d += (fresh[k] - decoded[o + k] as i32).abs();
                }
                let d = (d / 3) as u32;
                total += d as u64;
                if d > 24 {
                    over += 1;
                }
                let bi = (y / 16) * bcols + x / 16;
                blocks[bi] += d;
            }
        }
        for (i, &b) in blocks.iter().enumerate() {
            if b > worst {
                worst = b;
                worst_at = ((i % bcols) * 16, (i / bcols) * 16);
            }
        }
        let px = (dw * dh) as u64;
        println!("  mean absolute difference {:.2} of 255", total as f64 / px as f64);
        println!("  {} pixels ({:.2}%) differ by more than 24", over, over as f64 * 100.0 / px as f64);
        println!(
            "  worst macroblock at {},{}: mean {:.1}",
            worst_at.0, worst_at.1, worst as f64 / 256.0
        );
        println!("  (the screen moves between the last frame and this capture, so a");
        println!("   small mean and a *localised* worst block is what a healthy stream looks like)");
    }

    // ---------------------------------------------------------------- floor
    // What a frame costs when the encoder is told to look at *nothing*. That is
    // the fixed per-frame price -- the source copy into libvpx's lookahead
    // buffer, the border extension, the walk over every macroblock to find they
    // are all inactive, and the bitstream -- and it is the ceiling on frame
    // rate that no amount of damage-tracking can move.
    if all || want("floor") {
        use rustdesk_ppc_agent::encode::Tune;
        println!("\n=== the fixed cost of a frame, by scale ===");
        for &factor in &[8i32, 4, 2, 1] {
            let (w, h) = ((cap.width / factor as usize) & !1, (cap.height / factor as usize) & !1);
            if w < 32 || h < 32 { continue; }
            for &(pname, profile) in &[("profile 0", 0u32), ("profile 3", 3)] {
                let tune = Tune { profile, ..Tune::default() };
                let mut enc = match Encoder::tuned(w, h, (1000 / (factor * factor)).max(120) as u32, tune) {
                    Ok(e) => e,
                    Err(e) => { println!("  {}x{}: refused: {}", w, h, e); continue; }
                };
                let mut img = I420::new(w, h);
                cap.to_i420(&mut img, factor);
                let _ = enc.encode(&img, 0, true);
                let _ = enc.encode(&img, 100, false);
                enc.active_none();          // nothing at all is active
                let mut floor = 0.0;
                for i in 0..3 {
                    let t = Instant::now();
                    let _ = enc.encode(&img, 200 + i * 100, false);
                    floor += ms(t);
                }
                println!(
                    "  {:>4}x{:<4} (1/{}) {} {} MBs: floor {:6.0} ms  => {:.1} fps ceiling",
                    w, h, factor, pname, enc.mb_rows() * enc.mb_cols(),
                    floor / 3.0, 3000.0 / floor.max(1.0)
                );
            }
        }
    }

    // ---------------------------------------------------------------- sweep
    // The encoder settings, swept against a real frame. Every one of these
    // costs picture quality in some way, so the point is to find out how much
    // wall-clock each is actually buying before spending any of it.
    if all || want("sweep") {
        use rustdesk_ppc_agent::encode::Tune;
        println!("\n=== encoder settings, 640x512, inter frames ===");
        let factor = 2i32;
        let (w, h) = ((cap.width / factor as usize) & !1, (cap.height / factor as usize) & !1);
        let mut img = I420::new(w, h);
        cap.to_i420(&mut img, factor);

        let base = Tune::default();
        let cases: &[(&str, Tune)] = &[
            ("baseline (profile 0, min_q 8)", base),
            ("profile 1  simple lf, bilinear", Tune { profile: 1, ..base }),
            ("profile 2  NO loop filter", Tune { profile: 2, ..base }),
            ("profile 3  no lf, full-pixel MC", Tune { profile: 3, ..base }),
            ("min_q 24", Tune { min_q: 24, ..base }),
            ("min_q 40", Tune { min_q: 40, ..base }),
            ("static_threshold 15000", Tune { static_threshold: 15000, ..base }),
            ("profile 3 + min_q 24", Tune { profile: 3, min_q: 24, ..base }),
            ("profile 3 + min_q 24 + thresh 15000",
             Tune { profile: 3, min_q: 24, static_threshold: 15000, ..base }),
        ];

        for (name, tune) in cases {
            let mut enc = match Encoder::tuned(w, h, 250, *tune) {
                Ok(e) => e,
                Err(e) => { println!("  {:38} refused: {}", name, e); continue; }
            };
            let _ = enc.encode(&img, 0, true);
            let _ = enc.encode(&img, 100, false);
            // Whole frame, and the same frame with an 80-macroblock patch, so
            // the fixed per-frame cost and the per-macroblock cost separate.
            let mut full = 0.0;
            for i in 0..2 {
                let t = Instant::now();
                let _ = enc.encode(&img, 200 + i * 100, false);
                full += ms(t);
            }
            full /= 2.0;
            enc.active_none();
            enc.active_rect(0, 0, 160, 128);
            let mut patch = 0.0;
            let mut bytes = 0;
            for i in 0..2 {
                let t = Instant::now();
                bytes = enc.encode(&img, 500 + i * 100, false).map(|f| f.data.len()).unwrap_or(0);
                patch += ms(t);
            }
            patch /= 2.0;
            println!("  {:38} full {:7.0} ms   patch(80MB) {:6.0} ms   {} B", name, full, patch, bytes);
        }
    }

    // ------------------------------------------------------------ full loop
    // What a realistic steady-state frame would cost with all of it together:
    // poll the damage, convert only the damaged rectangles, encode with the
    // active map set from the same rectangles.
    if all || want("loop") {
        println!("\n=== steady state, damage-driven ===");
        for &factor in &[4i32, 2] {
            let (w, h) = ((cap.width / factor as usize) & !1, (cap.height / factor as usize) & !1);
            let kbps = (1000 / (factor * factor)).max(120) as u32;
            let mut enc = match Encoder::new(w, h, kbps) {
                Ok(e) => e,
                Err(e) => { println!("  encoder refused: {}", e); continue; }
            };
            let mut img = I420::new(w, h);
            cap.invalidate();
            let _ = cap.poll_rects();
            cap.to_i420(&mut img, factor);
            let _ = enc.encode(&img, 0, true);

            println!("  1/{} -> {}x{}: polling for 10 s, one line per frame", factor, w, h);
            let start = Instant::now();
            let mut frames = 0;
            let mut bytes = 0usize;
            while start.elapsed().as_secs() < 10 {
                let t = Instant::now();
                let rects: Vec<_> = cap.poll_rects().to_vec();
                let poll = ms(t);
                if rects.is_empty() {
                    continue;
                }
                let t = Instant::now();
                enc.active_none();
                for r in &rects {
                    let (x0, y0) = (r.x / factor, r.y / factor);
                    let (x1, y1) = ((r.x + r.w + factor - 1) / factor, (r.y + r.h + factor - 1) / factor);
                    cap.to_i420_rect(&mut img, factor, x0, y0, x1, y1);
                    enc.active_rect(x0, y0, x1 - x0, y1 - y0);
                }
                let conv = ms(t);
                let act = enc.active_count();
                let t = Instant::now();
                let n = enc.encode(&img, start.elapsed().as_millis() as i64, false)
                    .map(|f| f.data.len()).unwrap_or(0);
                let encms = ms(t);
                frames += 1;
                bytes += n;
                println!(
                    "    {} rect(s), poll {:.0} conv {:.0} enc {:.0} = {:.0} ms, {} MB(s), {} B",
                    rects.len(), poll, conv, encms, poll + conv + encms, act, n
                );
            }
            println!("    {} frames in 10 s, {} bytes", frames, bytes);
        }
    }

    println!("\nperfprobe done");
}

#[cfg(not(target_os = "irix"))]
fn main() {
    println!("perfprobe is for the IRIX target only");
}
