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
    --config PATH    config file (default ~/.rustdesk-ppc-agent.conf)",
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
            "-h" | "--help" => usage(),
            _ => usage(),
        }
    }

    // Minimal logger rather than env_logger, to keep the dependency set small.
    // Static, so it needs neither an allocation nor log's `std` feature.
    log::set_logger(&LOGGER).ok();
    log::set_max_level(log::LevelFilter::Info);

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
    };
    println!("agent id  : {}", ident.id);
    println!("public key: {}", base64(&pk.0));
    println!("display   : {}x{}", width, height);

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
            println!("display  : {}x{}  stride {}", c.width, c.height, c.stride());
            let tcap = std::time::Instant::now();
            let f = c.frame();
            let cap_ms = tcap.elapsed().as_millis();
            println!("first px : {:02x} {:02x} {:02x} {:02x}", f[0], f[1], f[2], f[3]);
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

            let (w, h, stride) = (c.width, c.height, c.stride());
            let t = std::time::Instant::now();
            let mut out = I420::new(w, h);
            argb_to_i420(f, stride, &mut out);
            let conv_ms = t.elapsed().as_millis();
            let total = cap_ms + conv_ms;
            println!("capture   : {} ms (VRAM -> RAM)", cap_ms);
            println!("argb->i420: {} ms", conv_ms);

            #[cfg(not(no_vpx))]
            let enc_ms = {
                match rustdesk_ppc_agent::encode::Encoder::new(out.width, out.height, 1500) {
                    Ok(mut e) => {
                        // First frame is a keyframe and not representative; time
                        // the second, which is what a steady session pays.
                        let _ = e.encode(&out, 0, true);
                        let t = std::time::Instant::now();
                        match e.encode(&out, 33, false) {
                            Ok(f) => {
                                let ms = t.elapsed().as_millis();
                                println!("vp8 encode: {} ms ({} bytes, key={})", ms, f.data.len(), f.key);
                                ms
                            }
                            Err(er) => { println!("vp8 encode: failed: {}", er); 0 }
                        }
                    }
                    Err(er) => { println!("vp8 encode: unavailable: {}", er); 0 }
                }
            };
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

struct StderrLogger;
impl log::Log for StderrLogger {
    fn enabled(&self, _: &log::Metadata) -> bool {
        true
    }
    fn log(&self, r: &log::Record) {
        eprintln!("[{}] {}", r.level(), r.args());
    }
    fn flush(&self) {}
}
