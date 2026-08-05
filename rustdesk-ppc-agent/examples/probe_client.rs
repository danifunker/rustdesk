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
            let text = match &expect_pk {
                Some(pkb) if pkb.len() == sign::PUBLICKEYBYTES => {
                    let mut k = [0u8; sign::PUBLICKEYBYTES];
                    k.copy_from_slice(pkb);
                    match sign::verify(&signed, &sign::PublicKey(k)) {
                        Ok(v) => {
                            println!("  signed_id signature: VERIFIED");
                            String::from_utf8(v).unwrap()
                        }
                        Err(_) => {
                            eprintln!("FAIL: signed_id signature did not verify");
                            std::process::exit(1)
                        }
                    }
                }
                _ => {
                    println!("  signed_id signature: not checked (no pubkey given)");
                    String::from_utf8_lossy(&signed[sign::SIGNATUREBYTES..]).into_owned()
                }
            };
            let mut parts = text.splitn(2, '\0');
            println!("  agent id  : {}", parts.next().unwrap_or(""));
            let raw = b64_decode(parts.next().unwrap_or("")).expect("bad ephemeral key b64");
            let mut pkb = [0u8; box_::PUBLICKEYBYTES];
            pkb.copy_from_slice(&raw);

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

    // 6. Watch the video stream, and poke the mouse to exercise injection.
    println!("\nwatching for video frames (15s)...");
    let start = Instant::now();
    let (mut frames, mut bytes, mut keys, mut other) = (0u32, 0usize, 0u32, 0u32);
    let mut first_frame_at: Option<Duration> = None;

    let mut poked = false;
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
                            }
                            if first_frame_at.is_none() {
                                first_frame_at = Some(start.elapsed());
                            }
                        }
                    }
                    _ => other += 1,
                },
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
}
