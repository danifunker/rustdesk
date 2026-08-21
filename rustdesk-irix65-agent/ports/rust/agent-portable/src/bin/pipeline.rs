//! Capture -> convert -> encode, end to end, timed on the machine itself.
//!
//! Every stage has been proved separately: capture against the live server,
//! the converters against each other, libvpx against its own round trip. What
//! nobody has measured is the three of them in a row, which is the only number
//! that answers "is this usable" — and the only place a byte-order mistake
//! between capture and convert would show up.
//!
//! Runs at full resolution and at 1/2 and 1/4 scale, because encode cost is
//! linear in pixel count and scale is the agent's main lever. Reports ms per
//! stage so the next optimisation goes where the time is.
//!
//! These are emulator numbers. A real R5000 Indy is roughly 3x faster and an O2
//! faster still; treat the ratios as sound and the absolutes as an upper bound.

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

    let mut cap = match Capturer::new() {
        Ok(c) => c,
        Err(e) => {
            println!("capture unavailable: {}", e);
            return;
        }
    };
    println!("capture: {} on {}x{}, screen depth {}, cursor embedded {}",
             cap.path_name(), cap.width, cap.height, cap.screen_depth(),
             cap.cursor_embedded());
    println!("cpus: {}", rustdesk_ppc_agent::sys::cpu_count());

    // One full read up front so the canvas holds a real screen rather than the
    // zeros it was allocated with; an all-black frame would flatter the encoder.
    let t = Instant::now();
    cap.frame();
    println!("initial full read: {:.0} ms\n", ms(t));

    // Scale factors come from the command line, because a full-resolution run
    // is not always safe: repeated 1280x1024 ReadDisplay reads have been seen to
    // wedge this emulated X server outright (it stops answering, keeps its
    // process, and stops accumulating CPU). Default to the small ones.
    let factors: Vec<i32> = {
        let a: Vec<String> = std::env::args().skip(1).collect();
        if a.is_empty() {
            vec![4, 2]
        } else {
            a.iter().filter_map(|s| s.parse().ok()).collect()
        }
    };
    println!("scale factors: {:?}\n", factors);

    for &factor in &factors {
        let (w, h) = (cap.width / factor as usize, cap.height / factor as usize);
        if w < 16 || h < 16 {
            continue;
        }
        println!("=== {}x{} (1/{}) ===", w, h, factor);

        // Bitrate roughly proportional to area, anchored at 1 Mbps full-screen.
        let kbps = (1000 / (factor * factor)).max(120) as u32;
        let mut enc = match Encoder::new(w, h, kbps) {
            Ok(e) => e,
            Err(e) => {
                println!("  encoder refused {}x{}: {}", w, h, e);
                continue;
            }
        };
        let mut i420 = I420::new(w, h);

        let mut t_cap = 0.0;
        let mut t_scale = 0.0;
        let mut t_conv = 0.0;
        let mut t_enc = 0.0;
        let mut bytes = 0usize;
        let rounds = 2;

        for i in 0..rounds {
            let t = Instant::now();
            cap.invalidate();
            let _ = cap.poll_rects();
            t_cap += ms(t);

            let t = Instant::now();
            let (src, sw, _sh, stride) = if factor == 1 {
                let s = cap.stride();
                (cap.buffer(), cap.width, cap.height, s)
            } else {
                let (b, dw, dh) = cap.scaled(factor);
                (b, dw, dh, dw * 4)
            };
            t_scale += ms(t);

            let t = Instant::now();
            argb_to_i420_rows(src, stride, &mut i420, 0, h & !1);
            t_conv += ms(t);
            let _ = sw;

            let t = Instant::now();
            match enc.encode(&i420, (i as i64) * 100, i == 0) {
                Ok(f) => bytes += f.data.len(),
                Err(e) => println!("  encode failed: {}", e),
            }
            t_enc += ms(t);
        }

        let r = rounds as f64;
        let total = (t_cap + t_scale + t_conv + t_enc) / r;
        println!("  capture  {:8.0} ms", t_cap / r);
        println!("  scale    {:8.0} ms", t_scale / r);
        println!("  convert  {:8.0} ms", t_conv / r);
        println!("  encode   {:8.0} ms", t_enc / r);
        println!("  ------------------");
        println!("  total    {:8.0} ms  =>  {:.2} fps, {} bytes/frame avg",
                 total, 1000.0 / total.max(1.0), bytes / rounds);

        // The same loop with damage polling instead of forced full reads, which
        // is what a real session looks like on a mostly static desktop.
        let t = Instant::now();
        let n = cap.poll_rects().len();
        println!("  idle poll {:7.0} ms ({} rect(s)) -- the steady-state cost\n",
                 ms(t), n);
    }
    println!("pipeline done");
}

#[cfg(not(target_os = "irix"))]
fn main() {
    println!("pipeline is for the IRIX target only");
}
