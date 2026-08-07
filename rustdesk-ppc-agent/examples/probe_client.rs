//! Connect to a running agent and report what actually happens.
//!
//! Drives the peer side of the protocol exactly as `src/client.rs` does, so it
//! exercises the real wire format rather than a mock — but unlike a real client
//! it narrates every step, which is what makes it useful for finding gaps.
//!
//! ```text
//! cargo run --example probe_client -- 192.168.99.116:21118 <password> [<pubkey-b64>]
//! ```
//!
//! The public key is optional: without it the `SignedId` signature is not
//! verified (the agent's identity is unchecked), which is fine for a probe on a
//! trusted network and lets you connect before copying the key across.

use std::io::{self, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

use protobuf::Message as _;
use rustdesk_ppc_agent::crypto::{expected_login_hash, SecureChannel};
use rustdesk_ppc_agent::frame::{read_frame, write_frame, DEFAULT_MAX_PACKET};
use rustdesk_ppc_agent::message_proto::*;
use sodiumoxide::crypto::{box_, secretbox, sign};

fn send(s: &mut TcpStream, ch: &mut Option<SecureChannel>, m: &Message) -> io::Result<()> {
    let body = m.write_to_bytes().unwrap();
    let body = match ch {
        Some(c) => c.seal(&body),
        None => body,
    };
    write_frame(s, &body)
}

fn recv(s: &mut TcpStream, ch: &mut Option<SecureChannel>) -> io::Result<Message> {
    let raw = read_frame(s, DEFAULT_MAX_PACKET)?;
    let body = match ch {
        Some(c) => c
            .open(&raw)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "decrypt failed"))?,
        None => raw,
    };
    Message::parse_from_bytes(&body)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))
}

fn b64_decode(s: &str) -> Option<Vec<u8>> {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let (mut out, mut acc, mut bits) = (Vec::new(), 0u32, 0u32);
    for ch in s.bytes() {
        if ch == b'=' {
            break;
        }
        let v = T.iter().position(|&c| c == ch)? as u32;
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 2 {
        eprintln!("usage: probe_client <host:port> <password> [<agent-pubkey-b64>]");
        std::process::exit(2);
    }
    let (addr, password) = (&a[0], &a[1]);
    let expect_pk = a.get(2).and_then(|s| b64_decode(s));

    println!("connecting to {} ...", addr);
    let mut s = TcpStream::connect(addr).expect("connect failed");
    s.set_nodelay(true).ok();
    s.set_read_timeout(Some(Duration::from_secs(20))).ok();
    let mut ch: Option<SecureChannel> = None;

    // 1. The agent either opens with signed_id (secure mode) or goes straight to
    //    hash (direct-IP mode, which is what a real client gets).
    let first = recv(&mut s, &mut ch).expect("no first message");
    let hash = match first.union {
        Some(message::Union::hash(h)) => {
            println!("  mode: direct-IP (no key exchange, UNENCRYPTED)");
            h
        }
        Some(message::Union::signed_id(x)) => {
            println!("  mode: secure (signed_id offered)");
            let signed = x.id;
            // Decoded exactly as `decode_id_pk` does in the client's
            // src/common.rs: verify, then parse the payload as `IdPk`. This
            // probe used to split the payload on a NUL and base64-decode the
            // rest, which is the 1.1.8 format -- so it agreed with the agent
            // while every real client fell back to plaintext. A mirror that
            // does not decode the way upstream decodes proves nothing.
            let payload = match &expect_pk {
                Some(pkb) if pkb.len() == sign::PUBLICKEYBYTES => {
                    let mut k = [0u8; sign::PUBLICKEYBYTES];
                    k.copy_from_slice(pkb);
                    match sign::verify(&signed, &sign::PublicKey(k)) {
                        Ok(v) => {
                            println!("  signed_id signature: VERIFIED");
                            v
                        }
                        Err(_) => {
                            eprintln!("FAIL: signed_id signature did not verify");
                            std::process::exit(1)
                        }
                    }
                }
                _ => {
                    println!("  signed_id signature: not checked (no pubkey given)");
                    signed[sign::SIGNATUREBYTES..].to_vec()
                }
            };
            let idpk = match IdPk::parse_from_bytes(&payload) {
                Ok(v) => v,
                Err(e) => {
                    // What a real client hits here it does not report: it
                    // answers with an empty PublicKey and carries on unencrypted.
                    eprintln!("FAIL: signed_id payload is not an IdPk protobuf: {}", e);
                    eprintln!("      a real client would silently drop to plaintext here");
                    std::process::exit(1)
                }
            };
            println!("  agent id  : {}", idpk.id);
            if idpk.pk.len() != box_::PUBLICKEYBYTES {
                eprintln!(
                    "FAIL: ephemeral key is {} bytes, not {}",
                    idpk.pk.len(),
                    box_::PUBLICKEYBYTES
                );
                std::process::exit(1)
            }
            let mut pkb = [0u8; box_::PUBLICKEYBYTES];
            pkb.copy_from_slice(&idpk.pk);

            let sym = secretbox::gen_key();
            let (our_pk, our_sk) = box_::gen_keypair();
            let sealed = box_::seal(
                &sym.0,
                &box_::Nonce([0u8; box_::NONCEBYTES]),
                &box_::PublicKey(pkb),
                &our_sk,
            );
            let mut pk = PublicKey::new();
            pk.asymmetric_value = our_pk.0.to_vec();
            pk.symmetric_value = sealed;
            let mut m = Message::new();
            m.set_public_key(pk);
            send(&mut s, &mut ch, &m).expect("send public_key");
            ch = Some(SecureChannel::new(sym));
            println!("  secure channel established");

            match recv(&mut s, &mut ch).expect("no hash").union {
                Some(message::Union::hash(h)) => h,
                _ => {
                    eprintln!("FAIL: expected hash");
                    std::process::exit(1)
                }
            }
        }
        other => {
            eprintln!("FAIL: unexpected first message (some={})", other.is_some());
            std::process::exit(1)
        }
    };

    // 2. Probe with an empty password exactly as a real client does, then send
    //    the real one on the SAME connection.
    let mut req = LoginRequest::new();
    req.my_id = "probe-client".into();
    req.my_name = "probe".into();
    let mut m = Message::new();
    m.set_login_request(req);
    send(&mut s, &mut ch, &m).expect("send empty-password probe");
    match recv(&mut s, &mut ch).expect("no reply to probe").union {
        Some(message::Union::login_response(r)) => match r.union {
            Some(login_response::Union::error(e)) => println!("  probe -> \"{}\" (expected \"Empty Password\")", e),
            Some(login_response::Union::peer_info(_)) => println!("  probe -> accepted with no password?!"),
            None => println!("  probe -> empty login_response"),
        },
        other => println!("  probe -> unexpected {:?}", other.is_some()),
    }

    // 3. LoginRequest with the real password, on the same connection
    let mut req = LoginRequest::new();
    req.my_id = "probe-client".into();
    req.my_name = "probe".into();
    req.password = expected_login_hash(password, hash.salt.as_bytes(), hash.challenge.as_bytes());
    let mut m = Message::new();
    m.set_login_request(req);
    send(&mut s, &mut ch, &m).expect("send login_request");

    // 5. LoginResponse
    match recv(&mut s, &mut ch).expect("no login_response").union {
        Some(message::Union::login_response(r)) => match r.union {
            Some(login_response::Union::peer_info(pi)) => {
                println!("  LOGIN OK");
                println!("    hostname : {}", pi.hostname);
                println!("    platform : {}", pi.platform);
                println!("    version  : {}", pi.version);
                for d in &pi.displays {
                    println!("    display  : {}x{} '{}'", d.width, d.height, d.name);
                }
            }
            Some(login_response::Union::error(e)) => {
                eprintln!("FAIL: login refused: {}", e);
                std::process::exit(1)
            }
            None => {
                eprintln!("FAIL: empty login_response");
                std::process::exit(1)
            }
        },
        _ => {
            eprintln!("FAIL: expected login_response");
            std::process::exit(1)
        }
    }

    // 6. Watch the video stream, poke the mouse, and press the refresh button.
    //
    // The refresh test carries a control, because the obvious version of it
    // always passes: the agent sends a keyframe every `KEYFRAME_INTERVAL` while
    // the settle repaint finishes a lap, so "a keyframe arrived after we asked"
    // is not on its own evidence that the asking caused it.
    // `Misc::refresh_video(false)` is a field the agent deliberately ignores --
    // it matches only on `true` -- so it measures what shows up anyway.
    println!("\nwatching for video frames (15s)...");
    let start = Instant::now();
    let (mut frames, mut bytes, mut keys, mut other) = (0u32, 0usize, 0u32, 0u32);
    let mut first_frame_at: Option<Duration> = None;
    let mut key_times: Vec<f64> = Vec::new();

    let mut poked = false;
    let (mut control_at, mut refresh_at) = (None, None);
    let mut shot_at: Option<f64> = None;
    let mut shot: Option<(f64, String, Vec<u8>)> = None;
    // The clipboard round trip needs two runs, because the agent suppresses the
    // echo of what it was just sent -- which is the whole point of `Sync`. Run
    // one sends a marker and the agent writes it to the Mac pasteboard; run two
    // is a fresh session with no memory, so the agent reads the pasteboard and
    // sends it back. Getting run one's marker back on run two proves the text
    // went through the real pasteboard and not through anything in this process.
    let clip_send = std::env::var("PROBE_CLIP").ok();
    let mut got_clip: Option<(&str, String)> = None;
    let mut clip_sent_at: Option<f64> = None;
    while start.elapsed() < Duration::from_secs(15) {
        match recv(&mut s, &mut ch) {
            Ok(msg) => match msg.union {
                Some(message::Union::video_frame(vf)) => match vf.union {
                    Some(video_frame::Union::vp8s(v)) | Some(video_frame::Union::vp9s(v)) => {
                        for f in &v.frames {
                            frames += 1;
                            bytes += f.data.len();
                            if f.key {
                                keys += 1;
                                key_times.push(start.elapsed().as_secs_f64());
                            }
                            if first_frame_at.is_none() {
                                first_frame_at = Some(start.elapsed());
                            }
                        }
                    }
                    _ => other += 1,
                },
                // Whatever is on the G5's clipboard. Proving this arrives is the
                // only way to check the pasteboard from off the machine: pbpaste
                // fails everywhere the agent can be reached from.
                Some(ref u @ message::Union::clipboard(_))
                | Some(ref u @ message::Union::multi_clipboards(_)) => {
                    let carrier = match u {
                        message::Union::clipboard(_) => "clipboard (16)",
                        _ => "multi_clipboards (28)",
                    };
                    match rustdesk_ppc_agent::clipboard::incoming_text(u) {
                        Some(t) => got_clip = Some((carrier, t)),
                        None => println!("  <- {} with no text in it", carrier),
                    }
                }
                Some(message::Union::screenshot_response(r)) => {
                    let at = shot_at.map(|t| start.elapsed().as_secs_f64() - t).unwrap_or(0.0);
                    shot = Some((at, r.msg, r.data.to_vec()));
                }
                _ => other += 1,
            },
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut => {}
            Err(e) => {
                println!("  stream ended: {}", e);
                break;
            }
        }
        // After 3s, move the mouse so input injection is exercised too.
        if !poked && start.elapsed() > Duration::from_secs(3) {
            poked = true;
            let mut me = MouseEvent::new();
            me.mask = 0; // move
            me.x = 400;
            me.y = 300;
            let mut m = Message::new();
            m.set_mouse_event(me);
            if send(&mut s, &mut ch, &m).is_ok() {
                println!("  sent a mouse-move to (400,300)");
            }
        }
        // The control first, then the real thing, each with room after it.
        if control_at.is_none() && start.elapsed() > Duration::from_secs(5) {
            let mut mi = Misc::new();
            mi.set_refresh_video(false);
            let mut m = Message::new();
            m.set_misc(mi);
            send(&mut s, &mut ch, &m).ok();
            control_at = Some(start.elapsed().as_secs_f64());
            println!("  sent Misc::refresh_video(false)      <- control, nothing should follow");
        }
        if refresh_at.is_none() && start.elapsed() > Duration::from_secs(8) {
            let mut mi = Misc::new();
            mi.set_refresh_video_display(0);
            let mut m = Message::new();
            m.set_misc(mi);
            send(&mut s, &mut ch, &m).ok();
            refresh_at = Some(start.elapsed().as_secs_f64());
            println!("  sent Misc::refresh_video_display(0)  <- the refresh button at 1.2.4+");
        }
        if clip_sent_at.is_none() && start.elapsed() > Duration::from_secs(10) {
            clip_sent_at = Some(start.elapsed().as_secs_f64());
            if let Some(text) = clip_send.as_deref() {
                // Addressed the way the agent addresses us: it reads our version
                // to choose, and we claim the same modern one a real client does.
                let m = rustdesk_ppc_agent::clipboard::outgoing(text, "1.4.5", "Linux");
                send(&mut s, &mut ch, &m).ok();
                println!("  sent clipboard \"{}\"", text);
            }
        }
        if shot_at.is_none() && start.elapsed() > Duration::from_secs(11) {
            let mut req = ScreenshotRequest::new();
            req.display = 0;
            req.sid = "probe".into();
            let mut m = Message::new();
            m.set_screenshot_request(req);
            send(&mut s, &mut ch, &m).ok();
            shot_at = Some(start.elapsed().as_secs_f64());
            println!("  sent ScreenshotRequest(display 0)    <- the button at 1.4.0+");
        }
        io::stdout().flush().ok();
    }

    let secs = start.elapsed().as_secs_f64();
    println!("\n--- results ---");
    println!("  video frames : {} ({} keyframes)", frames, keys);
    println!("  bytes        : {} ({:.0} KB/s)", bytes, bytes as f64 / 1024.0 / secs);
    println!("  frame rate   : {:.2} fps", frames as f64 / secs);
    match first_frame_at {
        Some(d) => println!("  first frame  : {:.2}s after login", d.as_secs_f64()),
        None => println!("  first frame  : NEVER  <-- no video was received"),
    }
    println!("  other msgs   : {}", other);

    // A keyframe that arrives promptly after a request is the request being
    // honoured; one that arrives promptly after the control is the settle
    // repaint's own cadence, and means this window is too coarse to conclude
    // anything from.
    const WINDOW: f64 = 2.0;
    let key_after = |t: f64| key_times.iter().find(|k| **k > t).map(|k| k - t);
    let timeline: Vec<String> = key_times.iter().map(|k| format!("{:.2}", k)).collect();
    println!("  keyframes at : [{}] s", timeline.join(", "));
    for (what, sent) in [
        ("refresh_video(false)    ", control_at),
        ("refresh_video_display(0)", refresh_at),
    ] {
        let Some(t) = sent else { continue };
        match key_after(t) {
            Some(d) if d <= WINDOW => println!("  {} -> keyframe after {:.2}s", what, d),
            _ => println!("  {} -> no keyframe within {:.1}s", what, WINDOW),
        }
    }

    match &got_clip {
        Some((carrier, t)) => println!("  clipboard    -> received via {}: \"{}\"", carrier, t),
        None => println!("  clipboard    -> nothing arrived from the G5"),
    }
    if let Some(sent) = clip_send.as_deref() {
        println!("  (sent \"{}\"; run again to see whether it comes back)", sent);
    }

    // The screenshot is written out rather than merely counted: whether the
    // bytes are a PNG a real decoder accepts is the whole question, and it
    // cannot be answered from this side of the wire.
    match shot {
        Some((at, msg, data)) if msg.is_empty() => {
            let path = std::env::var("PROBE_SHOT").unwrap_or_else(|_| "/tmp/probe-shot.png".into());
            match std::fs::write(&path, &data) {
                Ok(()) => println!(
                    "  screenshot   -> {} bytes after {:.2}s, written to {}",
                    data.len(),
                    at,
                    path
                ),
                Err(e) => println!("  screenshot   -> {} bytes, could not write: {}", data.len(), e),
            }
        }
        Some((at, msg, _)) => println!("  screenshot   -> refused after {:.2}s: \"{}\"", at, msg),
        None if shot_at.is_some() => println!("  screenshot   -> NO REPLY  <-- the request was dropped"),
        None => {}
    }
}
