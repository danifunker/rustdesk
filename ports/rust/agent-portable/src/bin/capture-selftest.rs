//! Drive the IRIX capture module from Rust on the machine it has to work on.
//!
//! capture_test.c already proved the C shim; this proves the Rust side of the
//! boundary — that the canvas slice is the right length and really contains the
//! server's pixels, that band mapping lines up with the rectangles the server
//! reported, and that a Capturer can be dropped and rebuilt, which is what the
//! agent does every time this X server is restarted.

#[cfg(target_os = "irix")]
fn main() {
    use rustdesk_ppc_agent::capture::{Capturer, BANDS, PATH_DAMAGE, PATH_GETIMAGE};
    use std::time::Instant;

    fn ms(t: Instant) -> f64 {
        let d = t.elapsed();
        d.as_secs() as f64 * 1000.0 + d.subsec_nanos() as f64 / 1.0e6
    }

    for &(name, path) in &[("damage", PATH_DAMAGE), ("fallback", PATH_GETIMAGE)] {
        println!("\n=== {} path ===", name);
        let mut c = match Capturer::open(None, path) {
            Ok(c) => c,
            Err(e) => {
                println!("  open failed: {}", e);
                continue;
            }
        };
        println!("  path            {}", c.path_name());
        println!("  geometry        {}x{}, screen depth {}, stride {}",
                 c.width, c.height, c.screen_depth(), c.stride());
        println!("  cursor embedded {}", c.cursor_embedded());
        println!("  band rows       {} over {} bands", c.band_rows(), BANDS);

        let t = Instant::now();
        let n = c.frame().len();
        println!("  frame()         {} bytes in {:.1} ms (expect {})",
                 n, ms(t), c.stride() * c.height);
        assert_eq!(n, c.stride() * c.height, "canvas slice is the wrong length");

        // The pixel at the top-left, so the A,B,G,R claim is visible rather than
        // asserted. Alpha should be 0xff on every ReadDisplay pixel.
        {
            let b = c.buffer();
            if b.len() >= 4 {
                println!("  first pixel     {:02x} {:02x} {:02x} {:02x}  (A,B,G,R)",
                         b[0], b[1], b[2], b[3]);
            }
        }

        let t = Instant::now();
        let rects: Vec<_> = c.poll_rects().to_vec();
        println!("  first poll      {} rect(s) in {:.1} ms", rects.len(), ms(t));
        if let Some(r) = rects.first() {
            println!("                  {:?}", r);
        }

        let mut idle = 0.0;
        let mut counts = 0;
        for _ in 0..5 {
            let t = Instant::now();
            counts += c.poll_rects().len();
            idle += ms(t);
        }
        println!("  5 idle polls    {:.1} ms total, {} rect(s) seen", idle, counts);

        let d = c.dirty_bands();
        println!("  dirty_bands     {} of {} set", d.iter().filter(|x| **x).count(), BANDS);

        c.invalidate();
        let d = c.dirty_bands();
        println!("  after invalidate {} of {} set (expect all)",
                 d.iter().filter(|x| **x).count(), BANDS);

        let t = Instant::now();
        let (buf, dw, dh) = c.scaled(2);
        println!("  scaled(2)       {}x{}, {} bytes in {:.1} ms", dw, dh, buf.len(), ms(t));

        println!("  dead?           {}", c.is_dead());
    }

    // Partial band mapping. The idle and invalidate cases only prove the two
    // extremes; this checks that damage confined to the top of the screen marks
    // the top bands and nothing else. It doubles as the first test of
    // std::process::Command on this target, which is its own port risk.
    println!("\n=== partial damage ===");
    match Capturer::new() {
        Err(e) => println!("  open failed: {}", e),
        Ok(mut c) => {
            c.invalidate();
            let _ = c.dirty_bands();          // drain the initial whole-screen report
            let _ = c.poll_rects();           // and settle

            let mut child = None;
            for prog in &["/usr/bin/X11/xclock", "/usr/bin/X11/xlogo"] {
                match std::process::Command::new(prog)
                    .arg("-geometry")
                    .arg("200x200+0+0")
                    .spawn()
                {
                    Ok(ch) => { println!("  spawned {} (pid {})", prog, ch.id()); child = Some(ch); break; }
                    Err(e) => println!("  {} did not start: {}", prog, e),
                }
            }
            if child.is_none() {
                println!("  no X client to draw with; skipping the partial check");
            } else {
                std::thread::sleep(std::time::Duration::from_secs(3));
                let d = c.dirty_bands();
                let set: Vec<usize> = d.iter().enumerate()
                    .filter(|(_, v)| **v).map(|(i, _)| i).collect();
                let rows = c.band_rows();
                println!("  bands dirty      {:?}", set);
                println!("  band rows        {} (a 200px-tall window spans bands 0..{})",
                         rows, 200 / rows);
                let confined = !set.is_empty()
                    && *set.iter().max().unwrap() <= (260 / rows)
                    && set.contains(&0);
                println!("  [{}] damage confined to the top of the screen",
                         if confined { "PASS" } else { "CHECK" });
                if let Some(mut ch) = child {
                    let _ = ch.kill();
                    let _ = ch.wait();
                    println!("  child reaped (fork/exec/wait all work here)");
                }
            }
        }
    }

    // Drop and rebuild, the sequence that follows every X server restart.
    println!("\n=== rebuild after drop ===");
    for i in 1..=3 {
        match Capturer::new() {
            Ok(mut c) => {
                let n = c.frame().len();
                println!("  build {}: ok, {} path, frame {} bytes", i, c.path_name(), n);
            }
            Err(e) => println!("  build {}: FAILED {}", i, e),
        }
    }
    println!("\ncapture self-test done");
}

#[cfg(not(target_os = "irix"))]
fn main() {
    println!("capture-selftest is for the IRIX target only");
}
