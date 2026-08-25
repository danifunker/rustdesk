//! captest -- what the capture path does on this machine, measured.
//!
//! Capture is the half of the agent that a remote desktop lives or dies by, and
//! it is the half that differs most between these ports. This exercises it
//! alone: no protocol, no crypto, no encoder, so a number it prints is about
//! the screen and nothing else.
//!
//!   captest                        # default display, best path
//!   captest --display :1           # a virtual server, for development
//!   captest --path getimage        # force the slow path, deliberately
//!   captest --frames 20 --ppm /tmp/screen.ppm
//!
//! The PPM is written raw rather than as a PNG on purpose: no encoder is in the
//! way of the pixels, so if red and blue come out swapped, the swap is the
//! capture's and not something downstream.

use std::io::Write;
use std::time::Instant;

use rustdesk_ppc_agent::capture::{self, Capturer, ORDER_ARGB, PATH_DAMAGE, PATH_GETIMAGE, PATH_SHM};

fn usage() -> ! {
    eprintln!(
        "captest -- exercise the Solaris capture path

USAGE:
    captest [--display :N] [--path damage|shm|getimage] [--frames N] [--ppm FILE]
            [--poll SECONDS]

OPTIONS:
    --display :N     X display (default $DISPLAY)
    --path P         refuse anything better than this path, to test a fallback
    --frames N       full-screen reads to time (default 10)
    --ppm FILE       write the captured screen here
    --poll SECONDS   then watch for damage for this long, reporting rectangles"
    );
    std::process::exit(2)
}

fn write_ppm(path: &str, buf: &[u8], w: usize, h: usize, stride: usize, order: i32) -> std::io::Result<()> {
    let mut out = Vec::with_capacity(w * h * 3 + 32);
    out.extend_from_slice(format!("P6\n{} {}\n255\n", w, h).as_bytes());
    // A,R,G,B on this machine; a little-endian server would hand back B,G,R,A
    // and the difference is exactly a swap of the two ends.
    let (ri, gi, bi) = if order == ORDER_ARGB { (1, 2, 3) } else { (2, 1, 0) };
    for y in 0..h {
        let row = &buf[y * stride..y * stride + w * 4];
        for x in 0..w {
            let p = &row[x * 4..x * 4 + 4];
            out.push(p[ri]);
            out.push(p[gi]);
            out.push(p[bi]);
        }
    }
    std::fs::File::create(path)?.write_all(&out)
}

fn main() {
    let mut display: Option<String> = None;
    let mut max_path = PATH_DAMAGE;
    let mut frames = 10usize;
    let mut ppm: Option<String> = None;
    let mut poll_secs = 0u64;

    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--display" if i + 1 < args.len() => { display = Some(args[i + 1].clone()); i += 2 }
            "--path" if i + 1 < args.len() => {
                max_path = match args[i + 1].as_str() {
                    "damage" => PATH_DAMAGE,
                    "shm" => PATH_SHM,
                    "getimage" => PATH_GETIMAGE,
                    other => { eprintln!("unknown path {}", other); usage() }
                };
                i += 2
            }
            "--frames" if i + 1 < args.len() => {
                frames = args[i + 1].parse().unwrap_or_else(|_| usage());
                i += 2
            }
            "--ppm" if i + 1 < args.len() => { ppm = Some(args[i + 1].clone()); i += 2 }
            "--poll" if i + 1 < args.len() => {
                poll_secs = args[i + 1].parse().unwrap_or_else(|_| usage());
                i += 2
            }
            "-h" | "--help" => usage(),
            other => { eprintln!("unexpected argument {}", other); usage() }
        }
    }

    match capture::display_size() {
        Some((w, h)) => println!("display_size      {}x{}", w, h),
        None => println!("display_size      unavailable"),
    }

    let mut cap = match Capturer::open(display.as_deref(), max_path) {
        Ok(c) => c,
        Err(e) => { eprintln!("captest: {}", e); std::process::exit(1) }
    };

    println!("screen            {}x{} depth {}", cap.width(), cap.height(), cap.screen_depth());
    println!("path              {} ({})", cap.path_name(), cap.path());
    println!("pixel order       {}", cap.pixel_order_name());
    println!("stride            {} bytes ({} would be unpadded)", cap.stride(), cap.width() * 4);
    println!("cursor embedded   {}", cap.cursor_embedded());

    // First read is separate: it is the one that faults in the shared segment,
    // so folding it into the average would flatter or slander every other read.
    let t0 = Instant::now();
    let first = cap.frame().len();
    println!("first full read   {:?} ({} bytes)", t0.elapsed(), first);

    let (mut best, mut total) = (f64::MAX, 0.0);
    for _ in 0..frames {
        let t = Instant::now();
        cap.frame();
        let ms = t.elapsed().as_secs_f64() * 1000.0;
        if ms < best { best = ms; }
        total += ms;
    }
    if frames > 0 {
        let mean = total / frames as f64;
        println!("{} full reads     best {:.1} ms, mean {:.1} ms  (=> {:.1} fps ceiling)",
                 frames, best, mean, 1000.0 / mean);
    }

    // An idle poll must not read a pixel. If this is not far cheaper than a
    // full read, the damage path is not doing its job.
    cap.invalidate();
    cap.poll_rects();
    let t = Instant::now();
    let mut idle = 0;
    for _ in 0..20 {
        if cap.poll_rects().is_empty() { idle += 1 }
    }
    println!("20 idle polls     {:?} total, {} reported nothing", t.elapsed(), idle);

    // --- the fused conversion, on a real frame ---------------------------
    //
    // `convtest` checks `to_i420_rect` against synthetic images, which is where
    // an arithmetic error shows. This is the other half: the same conversion on
    // pixels that actually came out of the framebuffer, through the whole
    // `Capturer` -- its stride, its buffer length, its pixel-order gate. Those
    // are what a synthetic test cannot reach, and getting the stride wrong on a
    // padded mode would look exactly like a working conversion here and a
    // sheared picture on the peer.
    //
    // At factor 1 the answer must be the shared converter's, and the luma must
    // be bit-identical -- the chroma differs by at most one code because the
    // shared one averages each channel before weighting it and this one weights
    // the sums. Anything larger than that, on a real desktop, is a red/blue
    // swap.
    {
        use rustdesk_ppc_agent::convert::{argb_to_i420, I420};
        cap.frame();
        let (w, h) = (cap.width(), cap.height());
        let mut want = I420::new(w, h);
        let tr = Instant::now();
        argb_to_i420(cap.buffer(), cap.stride(), &mut want);
        let ref_ms = tr.elapsed().as_secs_f64() * 1000.0;

        let mut got = I420::new(w, h);
        let tf = Instant::now();
        let ok = cap.to_i420(&mut got, 1);
        let fused_ms = tf.elapsed().as_secs_f64() * 1000.0;

        let dmax = |a: &[u8], b: &[u8]| -> i32 {
            let mut m = 0;
            for i in 0..a.len().min(b.len()) {
                let d = (a[i] as i32 - b[i] as i32).abs();
                if d > m { m = d }
            }
            m
        };
        let (dy, du, dv) = (dmax(&want.y, &got.y), dmax(&want.u, &got.u), dmax(&want.v, &got.v));
        println!(
            "to_i420 factor 1  {} in {:.0} ms (shared converter {:.0} ms); \
             vs it: Y max {} U max {} V max {}  {}",
            if ok { "converted" } else { "REFUSED" }, fused_ms, ref_ms, dy, du, dv,
            if ok && dy == 0 && du <= 1 && dv <= 1 { "ok" } else { "FAIL" }
        );

        // And the downscales, timed on this screen. Nothing to compare them
        // against here -- a box filter is not the same picture -- but the cost
        // is the number `DEFAULT_SCALE` should be chosen from, and it has never
        // been measured on this machine.
        for &f in [2i32, 4].iter() {
            let mut small = I420::new(w / f as usize, h / f as usize);
            let t = Instant::now();
            let ok = cap.to_i420(&mut small, f);
            println!(
                "to_i420 factor {}  {}x{} in {:.0} ms {}",
                f, small.width, small.height,
                t.elapsed().as_secs_f64() * 1000.0,
                if ok { "" } else { " REFUSED" }
            );
        }
    }

    if let Some(path) = ppm {
        cap.frame();
        match write_ppm(&path, cap.buffer(), cap.width(), cap.height(), cap.stride(), cap.pixel_order()) {
            Ok(()) => println!("wrote             {}", path),
            Err(e) => eprintln!("captest: writing {}: {}", path, e),
        }
    }

    if poll_secs > 0 {
        println!("watching for damage for {}s -- move something on screen", poll_secs);
        let until = Instant::now();
        let (mut polls, mut changed, mut rects) = (0u64, 0u64, 0usize);
        while until.elapsed().as_secs() < poll_secs {
            let n = cap.poll_rects().len();
            polls += 1;
            if n > 0 {
                changed += 1;
                rects += n;
                if changed <= 5 {
                    let r = cap.last_rects()[0];
                    println!("  damage: {} rect(s), first {}x{} at {},{}", n, r.w, r.h, r.x, r.y);
                }
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        println!("{} polls, {} saw change, {} rectangles total", polls, changed, rects);
        // The path can change under us: the damage path checks itself against
        // the canvas, and drops itself if the server was not reporting.
        println!("path now          {} ({})", cap.path_name(), cap.path());
        let note = cap.last_error();
        if !note.is_empty() {
            println!("note              {}", note);
        }
    }

    if cap.is_dead() {
        eprintln!("captest: the X connection died: {}", cap.last_error());
        std::process::exit(1);
    }
}
