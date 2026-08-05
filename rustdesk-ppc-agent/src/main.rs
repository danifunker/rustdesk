//! `rustdesk-agent` — the controlled side, for PowerPC Mac OS X 10.4/10.5.
//!
//! Direct-IP only: a peer connects to this machine's address and port, so no
//! rendezvous/ID server is needed. Its identity is the Ed25519 public key
//! printed by `--show-key`, which the peer must know out of band.

use std::process::exit;

use rustdesk_ppc_agent::{config::Config, session};

const DEFAULT_PORT: u16 = 21118;

fn usage() -> ! {
    eprintln!(
        "rustdesk-agent {} — RustDesk agent for PowerPC Mac OS X

USAGE:
    rustdesk-agent [--listen ADDR] [--port N]
    rustdesk-agent --password PASS
    rustdesk-agent --show-id | --show-key
    rustdesk-agent --probe-display

OPTIONS:
    --listen ADDR    bind address (default 0.0.0.0)
    --port N         bind port (default {})
    --password PASS  set the permanent password and exit
    --show-id        print this machine's agent ID and exit
    --show-key       print the public key a peer needs, and exit
    --probe-display  report what the framebuffer looks like, and exit
    --probe-live     watch the framebuffer for change and self-test the mouse
    --probe-keys [X Y]  click at X,Y to take focus, type, photograph (~/keys.ppm)
    --config PATH    config file (default ~/.rustdesk-ppc-agent.conf)
    --secure         require the signed_id/public_key exchange. OFF by default:
                     a client connecting by IP does not take part, and enabling
                     it there deadlocks the handshake.
    --log LEVEL      error | warn | info | debug | trace   (default info)
    -v               same as --log debug
    -vv              same as --log trace

Use --log trace to see every frame and message during a handshake; that is the
fastest way to find where a client diverges.",
        env!("CARGO_PKG_VERSION"),
        DEFAULT_PORT
    );
    exit(2)
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut listen = "0.0.0.0".to_owned();
    let mut port = DEFAULT_PORT;
    let mut cfg_path = Config::default_path();
    let mut set_password: Option<String> = None;
    let (mut show_id, mut show_key, mut probe) = (false, false, false);
    let mut probe_live = false;
    let mut probe_keys = false;
    let mut probe_keys_at: Option<(i32, i32)> = None;
    let mut level = log::LevelFilter::Info;
    let mut secure = false;

    let mut i = 0;
    while i < argv.len() {
        let need = |i: usize| -> String { argv.get(i + 1).cloned().unwrap_or_else(|| usage()) };
        match argv[i].as_str() {
            "--listen" => {
                listen = need(i);
                i += 2;
            }
            "--port" => {
                port = need(i).parse().unwrap_or_else(|_| usage());
                i += 2;
            }
            "--password" => {
                set_password = Some(need(i));
                i += 2;
            }
            "--config" => {
                cfg_path = need(i).into();
                i += 2;
            }
            "--show-id" => {
                show_id = true;
                i += 1;
            }
            "--show-key" => {
                show_key = true;
                i += 1;
            }
            "--probe-display" => {
                probe = true;
                i += 1;
            }
            "--probe-live" => {
                probe_live = true;
                i += 1;
            }
            "--probe-keys" => {
                probe_keys = true;
                // Optional click target: typing tests nothing if the focus is
                // somewhere that does not echo.
                if let (Some(x), Some(y)) = (argv.get(i + 1), argv.get(i + 2)) {
                    if let (Ok(x), Ok(y)) = (x.parse::<i32>(), y.parse::<i32>()) {
                        probe_keys_at = Some((x, y));
                        i += 2;
                    }
                }
                i += 1;
            }
            "--log" => {
                level = match need(i).to_ascii_lowercase().as_str() {
                    "error" => log::LevelFilter::Error,
                    "warn" => log::LevelFilter::Warn,
                    "info" => log::LevelFilter::Info,
                    "debug" => log::LevelFilter::Debug,
                    "trace" => log::LevelFilter::Trace,
                    _ => usage(),
                };
                i += 2;
            }
            "-v" => {
                level = log::LevelFilter::Debug;
                i += 1;
            }
            "-vv" => {
                level = log::LevelFilter::Trace;
                i += 1;
            }
            "--secure" => {
                secure = true;
                i += 1;
            }
            "-h" | "--help" => usage(),
            _ => usage(),
        }
    }

    // Minimal logger rather than env_logger, to keep the dependency set small.
    // Static, so it needs neither an allocation nor log's `std` feature.
    START_MS.store(now_ms(), std::sync::atomic::Ordering::Relaxed);
    log::set_logger(&LOGGER).ok();
    log::set_max_level(level);

    let mut cfg = Config::load(cfg_path);

    if let Some(p) = set_password {
        if p.len() < 6 {
            eprintln!("error: password must be at least 6 characters");
            exit(1);
        }
        cfg.set_password(&p).unwrap_or_else(|e| {
            eprintln!("error: could not save config: {}", e);
            exit(1);
        });
        println!("password set");
        return;
    }
    if show_id {
        println!("{}", cfg.id());
        return;
    }
    if show_key {
        let (pk, _) = cfg.key_pair();
        println!("{}", base64(&pk.0));
        return;
    }
    if probe {
        probe_display();
        return;
    }
    if probe_live {
        probe_live_fn();
        return;
    }
    if probe_keys {
        probe_keys_fn(probe_keys_at);
        return;
    }

    if cfg.password().is_empty() {
        eprintln!(
            "error: no password set. The agent will not accept connections without one.\n\
             \n    rustdesk-agent --password <PASSWORD>\n"
        );
        exit(1);
    }

    let (width, height) = display_size();
    let (pk, sk) = cfg.key_pair();
    let ident = session::Identity {
        id: cfg.id(),
        salt: cfg.salt(),
        password: cfg.password(),
        secret_key: sk,
        hostname: hostname(),
        width,
        height,
        secure,
    };
    println!("agent id  : {}", ident.id);
    println!("public key: {}", base64(&pk.0));
    println!("display   : {}x{}", width, height);
    println!("mode      : {}", if secure { "secure (peer must know our key)" } else { "direct-IP, UNENCRYPTED" });

    // Answer the broadcast that populates a client's local-network list. Its
    // own thread: see `lan` for why it is not in the session loop.
    {
        let me = rustdesk_ppc_agent::lan::Announcement {
            id: ident.id.clone(),
            hostname: ident.hostname.clone(),
            username: std::env::var("USER").unwrap_or_else(|_| "admin".to_owned()),
        };
        std::thread::spawn(move || rustdesk_ppc_agent::lan::serve(me));
    }

    if let Err(e) = session::listen(&format!("{}:{}", listen, port), &ident) {
        eprintln!("error: {}", e);
        exit(1);
    }
}

#[cfg(target_os = "macos")]
fn display_size() -> (i32, i32) {
    match rustdesk_ppc_agent::capture::Capturer::new() {
        Ok(c) => (c.width as i32, c.height as i32),
        Err(e) => {
            eprintln!("warning: could not read the display ({}); reporting 0x0", e);
            (0, 0)
        }
    }
}
#[cfg(not(target_os = "macos"))]
fn display_size() -> (i32, i32) {
    (0, 0)
}

#[cfg(target_os = "macos")]
fn probe_display() {
    use rustdesk_ppc_agent::convert::{argb_to_i420, I420};
    match rustdesk_ppc_agent::capture::Capturer::new() {
        Ok(mut c) => {
            let (w, h, stride) = (c.width, c.height, c.stride());
            println!("display  : {}x{}  stride {}", w, h, stride);

            // Capture and convert before anything else touches the capturer.
            // `frame()` borrows it for as long as the returned slice lives, so
            // the probes below cannot run until the conversion that reads it is
            // done. (mrustc does not borrow-check, so getting this wrong here
            // builds happily for the G5 and only fails under real rustc.)
            let mut out = I420::new(w, h);
            let tcap = std::time::Instant::now();
            let f = c.frame();
            let cap_ms = tcap.elapsed().as_millis();
            let first_px = format!("{:02x} {:02x} {:02x} {:02x}", f[0], f[1], f[2], f[3]);
            let tconv = std::time::Instant::now();
            argb_to_i420(f, stride, &mut out);
            let conv_ms = tconv.elapsed().as_millis();

            // The C shim is only worth having if it agrees with the Rust it
            // was ported from, and the way it would disagree -- swapped red and
            // blue -- looks fine until someone views a real desktop. Compare
            // the planes on a real frame, here, on the machine itself.
            let mut reference = I420::new(w, h);
            let tref = std::time::Instant::now();
            rustdesk_ppc_agent::convert::argb_to_i420_rows_rust(f, stride, &mut reference, 0, h);
            let conv_rust_ms = tref.elapsed().as_millis();
            let agrees =
                reference.y == out.y && reference.u == out.u && reference.v == out.v;

            println!("first px : {}", first_px);
            // How the real loop will behave: probe, then read only what moved.
            let t = std::time::Instant::now();
            let d1 = c.dirty_bands();
            let probe_ms = t.elapsed().as_millis();
            let t = std::time::Instant::now();
            let d2 = c.dirty_bands();
            let probe2_ms = t.elapsed().as_millis();
            println!("probe     : {} ms first ({} bands dirty), {} ms second ({} dirty)",
                probe_ms, d1.iter().filter(|x| **x).count(),
                probe2_ms, d2.iter().filter(|x| **x).count());

            // What the probe costs against how much of the screen it looks at.
            // Measured on the G5 this is flatly proportional to bytes -- window
            // size makes no difference once the sampled runs are `memcpy`'d out
            // rather than read byte by byte -- so these rows are a coverage
            // dial, and the rate column says whether the copy is running at the
            // ~23 MB/s a bulk read of VRAM gets. The first row is the shape the
            // probe had before any of this.
            println!("probe shape: ms per probe, median of 3");
            println!("            {:>6} {:>6} {:>8} {:>6} {:>8}", "window", "step", "KB", "ms", "MB/s");
            // The first group is a coverage dial at a fixed window. The second
            // holds coverage at 25% and varies the window, which is the
            // question that matters for typing: total bytes say nothing about
            // whether a 7-pixel character is seen, because what hides one is
            // the *gap* between windows. 128-in-512 leaves a 96-pixel gap --
            // thirteen characters can be typed into it unnoticed.
            for &(window, step) in &[
                (3usize, 64usize),
                (128, 2048),
                (128, 1024),
                (128, 512),
                (128, 256),
                (stride, stride),
                // same 25% coverage, finer and finer:
                (64, 256),
                (32, 128),
                (16, 64),
                (8, 32),
            ] {
                let mut ms = [0u128; 3];
                for m in ms.iter_mut() {
                    let t = std::time::Instant::now();
                    c.dirty_bands_tuned(window, step);
                    *m = t.elapsed().as_millis();
                }
                ms.sort();
                let kb = c.probe_bytes(window, step) / 1024;
                let rate = if ms[1] > 0 { (kb as u128 * 1000 / ms[1] / 1024).to_string() } else { "-".to_owned() };
                println!("            {:>6} {:>6} {:>8} {:>6} {:>8}", window, step, kb, ms[1], rate);
            }
            c.invalidate(); // the sweep left the checksums describing its last pattern

            let total = cap_ms + conv_ms;
            println!("capture   : {} ms (VRAM -> RAM)", cap_ms);
            println!(
                "argb->i420: {} ms (C shim) vs {} ms (rust reference) -- planes {}",
                conv_ms,
                conv_rust_ms,
                if agrees { "identical" } else { "*** DIFFER: the shim is wrong ***" }
            );

            #[cfg(not(no_vpx))]
            let enc_ms = encode_sweep(&mut out);
            #[cfg(no_vpx)]
            let enc_ms = 0;

            let total = total + enc_ms;
            println!("total     : {} ms ({} fps) for a full-screen change", total,
                if total > 0 { (1000 / total).to_string() } else { "inf".to_owned() });
            println!("idle cost : {} ms (probe only)", probe2_ms);
        }
        Err(e) => println!("capture unavailable: {}", e),
    }
}
#[cfg(not(target_os = "macos"))]
fn probe_display() {
    println!("--probe-display is only meaningful on macOS");
}

/// Time each encoder tuning against a still frame and a small change, and
/// return what the shipping defaults cost.
///
/// Two columns because they answer different questions. **still** re-encodes an
/// identical frame: it is the price of concluding that nothing moved, which the
/// session pays on every frame it sends however little changed, and it is the
/// floor that makes VP8 rather than capture the limit on ordinary interaction.
/// **small** repaints a 256x64 patch first, which is roughly a redrawn line or
/// two of text.
///
/// Median of three, because a single sample on a machine with a window server
/// on it is noise. Each row builds its own encoder: every knob here is fixed at
/// `vpx_codec_enc_init` time or depends on state accumulated since it.
#[cfg(all(target_os = "macos", not(no_vpx)))]
fn encode_sweep(img: &mut rustdesk_ppc_agent::convert::I420) -> u128 {
    use rustdesk_ppc_agent::encode::{Encoder, Tune};

    // Spelled out rather than derived from `Tune::default()`, so that moving a
    // default does not quietly change what any row of the sweep means. `plain`
    // is where this started, before any of it was measured.
    let plain = Tune { static_threshold: 1000, last_ref_only: false, error_resilient: true };
    let sweep: &[(&str, Tune)] = &[
        ("static 1000", plain),
        ("static 6000", Tune { static_threshold: 6000, ..plain }),
        ("static 15000", Tune { static_threshold: 15000, ..plain }),
        ("static 30000", Tune { static_threshold: 30000, ..plain }),
        ("last ref only", Tune { last_ref_only: true, ..plain }),
        ("no err resil", Tune { error_resilient: false, ..plain }),
        ("15000 + lastref", Tune { static_threshold: 15000, last_ref_only: true, ..plain }),
        (
            "15000 + both",
            Tune { static_threshold: 15000, last_ref_only: true, error_resilient: false },
        ),
    ];

    println!("vp8 tuning: ms per frame, median of 3 (bytes for the small change)");
    println!("            {:<16} {:>5} {:>6} {:>8}", "config", "still", "small", "bytes");
    // Every configuration has to start from the same picture, so stamp the
    // patch on once here rather than leaving the first row encoding the clean
    // screen and the rest encoding a patched one.
    dirty_patch(img, 0);
    let mut baseline_ms = 0;
    for (label, tune) in sweep {
        let mut e = match Encoder::tuned(img.width, img.height, 1500, *tune) {
            Ok(e) => e,
            Err(er) => {
                println!("            {:<16} unavailable: {}", label, er);
                continue;
            }
        };
        // The first frame is a keyframe and nothing like what a steady session
        // pays; the few after it are still settling. Encode and discard.
        let _ = e.encode(img, 0, true);
        let mut pts = 33i64;
        for _ in 0..3 {
            let _ = e.encode(img, pts, false);
            pts += 33;
        }

        // Alternate the two cases rather than timing one column and then the
        // other. Rate control and mode decisions carry from frame to frame, so
        // whichever column runs second is measured against a different encoder
        // state -- with them run in sequence a still frame came out dearer than
        // a changed one, which is not a thing that can be true.
        let mut still = [0u128; 3];
        let mut small = [0u128; 3];
        let mut bytes = 0usize;
        for i in 0..3 {
            let t = std::time::Instant::now();
            let _ = e.encode(img, pts, false);
            still[i] = t.elapsed().as_millis();
            pts += 33;

            dirty_patch(img, i as u8 + 1);
            let t = std::time::Instant::now();
            bytes = match e.encode(img, pts, false) {
                Ok(f) => f.data.len(),
                Err(_) => 0,
            };
            small[i] = t.elapsed().as_millis();
            pts += 33;
        }
        // Back to pattern 0, so the next config sees what this one first saw.
        dirty_patch(img, 0);

        still.sort();
        small.sort();
        let mark = if *tune == Tune::default() { "  <- in use" } else { "" };
        println!(
            "            {:<16} {:>5} {:>6} {:>8}{}",
            label, still[1], small[1], bytes, mark
        );
        if *tune == Tune::default() {
            baseline_ms = still[1];
        }
    }
    baseline_ms
}

/// Repaint a 256x64 patch of the luma plane with something text-shaped.
///
/// Dark runs on a light background, in rows of twelve -- crude, but closer to
/// what a terminal redraw costs an encoder than random noise would be. Noise is
/// the worst case for a codec and would make every configuration here look bad
/// in the same way.
#[cfg(all(target_os = "macos", not(no_vpx)))]
fn dirty_patch(img: &mut rustdesk_ppc_agent::convert::I420, n: u8) {
    let (pw, ph) = (256.min(img.width), 64.min(img.height));
    for y in 0..ph {
        let line = (y + n as usize) % 12;
        for x in 0..pw {
            let ink = line >= 3 && line < 11 && (x + n as usize) % 7 < 4;
            img.y[y * img.width + x] = if ink { 40 } else { 220 };
        }
    }
}

/// Answers the two open questions at once: does the framebuffer reflect changes,
/// and does injected input reach the window server?
/// Type a string the way a client does and photograph the result.
///
/// Keyboard injection is otherwise unverifiable from here: unlike the mouse,
/// which reports its position back, a keystroke leaves no trace unless something
/// focused renders it. Spotlight is always available and always shows what it
/// was given, so Cmd+Space then a known word makes the result readable off the
/// screen. Goes through `Injector` deliberately -- this exercises the same path
/// the session loop uses, `chr` values and all.
#[cfg(target_os = "macos")]
fn probe_keys_fn(click_at: Option<(i32, i32)>) {
    use protobuf::ProtobufEnumOrUnknown;
    use rustdesk_ppc_agent::message_proto::MouseEvent;
    use rustdesk_ppc_agent::input::Injector;
    use rustdesk_ppc_agent::message_proto::{key_event, ControlKey, KeyEvent};

    let mut c = match rustdesk_ppc_agent::capture::Capturer::new() {
        Ok(c) => c,
        Err(e) => {
            println!("capture unavailable: {}", e);
            return;
        }
    };
    let mut inj = Injector::new();

    let sleep = |ms| std::thread::sleep(std::time::Duration::from_millis(ms));
    let mut send = |inj: &mut Injector, u: key_event::Union, down: bool, mods: &[ControlKey]| {
        let mut k = KeyEvent::new();
        k.down = down;
        k.union = Some(u);
        for m in mods {
            k.modifiers.push(ProtobufEnumOrUnknown::new(*m));
        }
        inj.key(&k);
    };

    // Type into whatever is focused, having first cleared the line with ctrl-C.
    //
    // Earlier versions aimed at Spotlight, then at Finder's Go-to-Folder sheet.
    // Both were fragile: the probe cannot tell what is frontmost, and the wrong
    // target leaves the result unreadable. A shell prompt echoes exactly what it
    // was sent, and a ctrl-C on each side leaves nothing behind. Note that
    // ctrl-C is itself part of the test -- it only works if a character composes
    // with its modifier into a real keycode.
    // Keystrokes go to whatever holds the keyboard focus, so a probe that only
    // types proves nothing about the mapping when the focus is somewhere that
    // does not echo. Click into the target first when one is given.
    if let Some((x, y)) = click_at {
        println!("clicking at {},{} to take focus", x, y);
        let mut m = MouseEvent::new();
        m.mask = 0;
        m.x = x;
        m.y = y;
        inj.mouse(&m);
        sleep(400);
        m.mask = (1 << 3) | 1;
        inj.mouse(&m);
        sleep(120);
        m.mask = (1 << 3) | 2;
        inj.mouse(&m);
        sleep(800);
    }

    // Report the mapping before using it: if this is empty, rd_key_char is
    // falling back to unicode entry and shortcuts will not compose, which looks
    // from the client end exactly like keys not arriving.
    println!("\ncharacter -> keycode, as the shim will type them:");
    let mut mapped = 0;
    for ch in "rustdesk R!".chars() {
        match rustdesk_ppc_agent::input::keycode_for_char(ch as u32) {
            Some((code, shift)) => {
                mapped += 1;
                println!("  {:?} -> keycode {}{}", ch, code, if shift { " + shift" } else { "" });
            }
            None => println!("  {:?} -> unmapped (falls back to unicode entry)", ch),
        }
    }
    println!("  {} of 11 mapped\n", mapped);

    println!("clearing the line (ctrl-C)");
    let ctrl_c = |inj: &mut Injector| {
        let mut k = KeyEvent::new();
        k.down = true;
        k.union = Some(key_event::Union::chr('c' as u32));
        k.modifiers.push(ProtobufEnumOrUnknown::new(ControlKey::Control));
        inj.key(&k);
        std::thread::sleep(std::time::Duration::from_millis(60));
        k.down = false;
        inj.key(&k);
    };
    ctrl_c(&mut inj);
    sleep(900);

    let word = "rustdesk";
    println!("typing {:?} as chr() codepoints, exactly as a client sends them", word);
    for ch in word.chars() {
        send(&mut inj, key_event::Union::chr(ch as u32), true, &[]);
        sleep(40);
        send(&mut inj, key_event::Union::chr(ch as u32), false, &[]);
        sleep(120);
    }
    sleep(1200);

    let (w, h, stride) = (c.width, c.height, c.stride());
    let frame = c.frame();
    let path = format!("{}/keys.ppm", std::env::var("HOME").unwrap_or_else(|_| ".".into()));
    match write_ppm(&path, frame, w, h, stride) {
        Ok(()) => println!("wrote {} -- {:?} should appear wherever the focus was", path, word),
        Err(e) => println!("could not write {}: {}", path, e),
    }

    println!("clearing up (ctrl-C)");
    ctrl_c(&mut inj);
}

#[cfg(not(target_os = "macos"))]
fn probe_keys_fn(_click_at: Option<(i32, i32)>) {
    println!("--probe-keys is only meaningful on macOS");
}

/// Dump an ARGB frame as a binary PPM. Memory order is A,R,G,B, so RGB is at +1.
#[cfg(target_os = "macos")]
fn write_ppm(path: &str, frame: &[u8], w: usize, h: usize, stride: usize) -> std::io::Result<()> {
    use std::io::Write;
    let mut f = std::io::BufWriter::new(std::fs::File::create(path)?);
    write!(f, "P6\n{} {}\n255\n", w, h)?;
    for y in 0..h {
        let row = &frame[y * stride..y * stride + w * 4];
        let mut out = Vec::with_capacity(w * 3);
        for p in row.chunks(4) {
            out.extend_from_slice(&p[1..4]);
        }
        f.write_all(&out)?;
    }
    f.flush()
}

#[cfg(target_os = "macos")]
fn probe_live_fn() {
    use rustdesk_ppc_agent::input::{cursor_position, Injector};
    use rustdesk_ppc_agent::message_proto::MouseEvent;

    let mut c = match rustdesk_ppc_agent::capture::Capturer::new() {
        Ok(c) => c,
        Err(e) => {
            println!("capture unavailable: {}", e);
            return;
        }
    };
    println!("display {}x{}", c.width, c.height);

    println!("\n--- mouse injection ---");
    let before = cursor_position();
    println!("  cursor before : {:.0},{:.0}", before.0, before.1);
    let mut inj = Injector::new();
    let (tx, ty) = ((c.width / 3) as i32, (c.height / 3) as i32);
    let mut ev = MouseEvent::new();
    ev.mask = 0;
    ev.x = tx;
    ev.y = ty;
    inj.mouse(&ev);
    std::thread::sleep(std::time::Duration::from_millis(400));
    let after = cursor_position();
    println!("  asked for     : {},{}", tx, ty);
    println!("  cursor after  : {:.0},{:.0}", after.0, after.1);
    let moved = (after.0 - tx as f64).abs() < 4.0 && (after.1 - ty as f64).abs() < 4.0;
    println!("  => injection {}", if moved { "WORKS" } else { "did NOT move the cursor" });

    // Cause the change we are looking for, rather than asking someone to stand
    // at the machine and wiggle a window. Opening the Apple menu repaints a
    // large, unmistakable region; Escape closes it again, which doubles as an
    // objective test that key injection reaches the window server -- there is no
    // other way to check keys without a focused text field to type into.
    println!("\n--- framebuffer liveness (10s) ---");
    println!("  opening the Apple menu at t=2s, closing it with Escape at t=6s");
    c.invalidate();
    let (mut seen_change, mut dirty_after_menu, mut dirty_after_escape) = (false, false, false);
    for i in 0..10 {
        match i {
            2 => {
                let mut m = MouseEvent::new();
                m.mask = 0;
                m.x = 20;
                m.y = 10;
                inj.mouse(&m);              // move onto the Apple menu
                m.mask = (1 << 3) | 1;      // left button, down
                inj.mouse(&m);
                m.mask = (1 << 3) | 2;      // left button, up
                inj.mouse(&m);
            }
            6 => {
                use rustdesk_ppc_agent::message_proto::{key_event, ControlKey, KeyEvent};
                let mut k = KeyEvent::new();
                k.down = true;
                k.press = true;
                k.union = Some(key_event::Union::control_key(
                    protobuf::ProtobufEnumOrUnknown::new(ControlKey::Escape),
                ));
                inj.key(&k);
            }
            _ => {}
        }

        let d = c.dirty_bands();
        let n = d.iter().filter(|x| **x).count();
        if i > 0 && n > 0 {
            seen_change = true;
        }
        if (i == 3 || i == 4) && n > 0 {
            dirty_after_menu = true;
        }
        if (i == 7 || i == 8) && n > 0 {
            dirty_after_escape = true;
        }
        println!("  t={:2}s  dirty bands: {}", i, n);
        std::thread::sleep(std::time::Duration::from_millis(1000));
    }
    println!(
        "  => framebuffer {}",
        if seen_change { "IS live" } else { "appears FROZEN (nothing changed after the first probe)" }
    );
    println!(
        "  => mouse click {}",
        if dirty_after_menu { "repainted the screen (menu opened)" } else { "changed nothing" }
    );
    println!(
        "  => key injection {}",
        if dirty_after_escape { "repainted the screen (menu closed)" } else { "changed nothing" }
    );
}
#[cfg(not(target_os = "macos"))]
fn probe_live_fn() {
    println!("--probe-live is only meaningful on macOS");
}

fn hostname() -> String {
    std::process::Command::new("hostname")
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_owned())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "mac".to_owned())
}

fn base64(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for c in b.chunks(3) {
        let x = [c[0], *c.get(1).unwrap_or(&0), *c.get(2).unwrap_or(&0)];
        let n = ((x[0] as u32) << 16) | ((x[1] as u32) << 8) | x[2] as u32;
        out.push(T[(n >> 18 & 63) as usize] as char);
        out.push(T[(n >> 12 & 63) as usize] as char);
        out.push(if c.len() > 1 { T[(n >> 6 & 63) as usize] as char } else { '=' });
        out.push(if c.len() > 2 { T[(n & 63) as usize] as char } else { '=' });
    }
    out
}

static LOGGER: StderrLogger = StderrLogger;

/// Milliseconds since the epoch at startup. An AtomicU64 rather than
/// LazyLock<Instant>, which is 1.80+ and mrustc targets 1.74.
static START_MS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn since_start() -> f64 {
    let s = START_MS.load(std::sync::atomic::Ordering::Relaxed);
    if s == 0 { 0.0 } else { (now_ms().saturating_sub(s)) as f64 / 1000.0 }
}

struct StderrLogger;
impl log::Log for StderrLogger {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::max_level()
    }
    fn log(&self, r: &log::Record) {
        if !self.enabled(r.metadata()) {
            return;
        }
        // Seconds since start rather than wall-clock: no chrono, and elapsed
        // time is what matters when reading a handshake trace.
        eprintln!("[{:>7.3}] {:<5} {}", since_start(), r.level(), r.args());
    }
    fn flush(&self) {}
}
