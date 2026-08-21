//! Connect to the agent as a real client would, and report what comes back.
//!
//! Everything so far has tested the agent's parts. This tests the agent: it
//! speaks the actual protocol over a socket -- signed identity, sealed
//! symmetric key, password hash, login -- and then sits in the message loop
//! counting video frames. If this prints frames, the app works.
//!
//! The handshake mirrors `session.rs`'s own tests, which in turn mirror what
//! upstream's client does, so a divergence here is a divergence from RustDesk.
//!
//!     testpeer <host:port> [password] [seconds]

use rustdesk_ppc_agent::crypto::{expected_login_hash, SecureChannel};
use rustdesk_ppc_agent::frame::{read_frame, write_frame};
use rustdesk_ppc_agent::message_proto::*;
use protobuf::Message as _;
use sodiumoxide::crypto::{box_, secretbox, sign};
use std::io;
use std::net::TcpStream;
use std::time::{Duration, Instant};

const MAX_PACKET: usize = 8 * 1024 * 1024;

struct Peer {
    stream: TcpStream,
    chan: Option<SecureChannel>,
}

impl Peer {
    fn send(&mut self, msg: &Message) -> io::Result<()> {
        let body = msg
            .write_to_bytes()
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
        let body = match self.chan.as_mut() {
            Some(c) => c.seal(&body),
            None => body,
        };
        write_frame(&mut self.stream, &body)
    }

    fn recv(&mut self) -> io::Result<Message> {
        let raw = read_frame(&mut self.stream, MAX_PACKET)?;
        let body = match self.chan.as_mut() {
            Some(c) => c
                .open(&raw)
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "decrypt failed"))?,
            None => raw,
        };
        Message::parse_from_bytes(&body)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let addr = args.get(0).cloned().unwrap_or_else(|| "127.0.0.1:21118".into());
    let password = args.get(1).cloned().unwrap_or_default();
    let secs: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(30);

    println!("connecting to {}", addr);
    let stream = match TcpStream::connect(&addr) {
        Ok(s) => s,
        Err(e) => {
            println!("connect failed: {}", e);
            return;
        }
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(120)));
    let mut c = Peer { stream, chan: None };
    println!("connected");

    // The agent opens in one of two ways, and a client has to cope with both:
    //
    //   signed_id  the agent has a key to prove and wants an encrypted channel
    //   hash       direct-IP mode, where it skips the key exchange entirely
    //
    // Assuming the first and getting the second is a deadlock: the peer waits
    // for a message the agent already decided not to send.
    let first = match c.recv() {
        Ok(m) => m,
        Err(e) => {
            println!("nothing from the agent: {}", e);
            return;
        }
    };
    let (signed, early_hash) = match first.union {
        Some(message::Union::signed_id(s)) => (Some(s.id), None),
        Some(message::Union::hash(h)) => {
            println!("agent opened with hash -- direct-IP mode, no encryption");
            (None, Some(h))
        }
        _ => {
            println!("agent opened with neither signed_id nor hash");
            return;
        }
    };

    let signed = match signed {
        Some(s) => s,
        None => {
            // Direct-IP: straight to the password.
            let hash = early_hash.unwrap();
            println!("hash: salt {} chars, challenge {} chars",
                     hash.salt.len(), hash.challenge.len());
            finish_login(&mut c, &hash, &password, secs);
            return;
        }
    };
    // sign::sign produces the combined form: signature followed by the message.
    // Without the agent's public key the signature cannot be checked, so read
    // the payload past the 64-byte signature.
    if signed.len() <= sign::SIGNATUREBYTES {
        println!("signed_id too short: {} bytes", signed.len());
        return;
    }
    let idpk = match IdPk::parse_from_bytes(&signed[sign::SIGNATUREBYTES..]) {
        Ok(v) => v,
        Err(e) => {
            println!("signed_id payload is not an IdPk: {}", e);
            return;
        }
    };
    println!("agent id '{}', ephemeral key {} bytes (signature not checked: this \
              probe has no out-of-band copy of the agent's public key)",
             idpk.id, idpk.pk.len());
    if idpk.pk.len() != box_::PUBLICKEYBYTES {
        println!("ephemeral key is the wrong length");
        return;
    }
    let mut pkb = [0u8; box_::PUBLICKEYBYTES];
    pkb.copy_from_slice(&idpk.pk);

    // 2. Seal a fresh symmetric key to it, zero nonce, exactly as upstream does.
    let sym = secretbox::gen_key();
    let (our_pk, our_sk) = box_::gen_keypair();
    let sealed = box_::seal(
        &sym.0,
        &box_::Nonce([0u8; box_::NONCEBYTES]),
        &box_::PublicKey(pkb),
        &our_sk,
    );
    let mut pkmsg = PublicKey::new();
    pkmsg.asymmetric_value = our_pk.0.to_vec();
    pkmsg.symmetric_value = sealed;
    let mut m = Message::new();
    m.set_public_key(pkmsg);
    if let Err(e) = c.send(&m) {
        println!("sending our key failed: {}", e);
        return;
    }
    c.chan = Some(SecureChannel::new(sym));
    println!("secure channel up");

    // 3. The agent's salt and challenge.
    let hash = match c.recv() {
        Ok(m) => match m.union {
            Some(message::Union::hash(h)) => h,
            _ => {
                println!("expected hash");
                return;
            }
        },
        Err(e) => {
            println!("no hash: {}", e);
            return;
        }
    };
    println!("hash: salt {} chars, challenge {} chars", hash.salt.len(), hash.challenge.len());

    finish_login(&mut c, &hash, &password, secs);
}

fn finish_login(c: &mut Peer, hash: &Hash, password: &str, secs: u64) {
    // Log in.
    let mut req = LoginRequest::new();
    req.my_id = "testpeer".into();
    req.my_name = "testpeer".into();
    req.password = if password.is_empty() {
        Vec::new()
    } else {
        expected_login_hash(password, hash.salt.as_bytes(), hash.challenge.as_bytes())
    };
    let mut m = Message::new();
    m.set_login_request(req);
    if let Err(e) = c.send(&m) {
        println!("login send failed: {}", e);
        return;
    }

    // 5. And see whether we are in.
    match c.recv() {
        Ok(m) => match m.union {
            Some(message::Union::login_response(r)) => match r.union {
                Some(login_response::Union::peer_info(pi)) => {
                    println!("LOGGED IN: {} displays, hostname '{}', platform '{}'",
                             pi.displays.len(), pi.hostname, pi.platform);
                    for d in pi.displays.iter() {
                        println!("  display '{}' {}x{} at {},{}, online {}",
                                 d.name, d.width, d.height, d.x, d.y, d.online);
                    }
                }
                Some(login_response::Union::error(e)) => {
                    println!("LOGIN REFUSED: {}", e);
                    return;
                }
                None => {
                    println!("empty login_response");
                    return;
                }
            },
            _ => {
                println!("expected login_response");
                return;
            }
        },
        Err(e) => {
            println!("no login_response: {}", e);
            return;
        }
    }

    // 6. Watch the video pump.
    println!("\nwatching for {} s ...", secs);
    let start = Instant::now();
    let (mut frames, mut bytes, mut keyframes, mut cursors, mut others) = (0u32, 0usize, 0u32, 0u32, 0u32);
    let mut first_frame_ms = None;
    let _ = c.stream.set_read_timeout(Some(Duration::from_secs(secs.max(5))));
    while start.elapsed() < Duration::from_secs(secs) {
        match c.recv() {
            Ok(m) => match m.union {
                Some(message::Union::video_frame(vf)) => {
                    if let Some(video_frame::Union::vp8s(vp8s)) = vf.union {
                        for f in vp8s.frames.iter() {
                            frames += 1;
                            bytes += f.data.len();
                            if f.key {
                                keyframes += 1;
                            }
                            if first_frame_ms.is_none() {
                                first_frame_ms = Some(start.elapsed().as_secs() as f64 * 1000.0
                                    + start.elapsed().subsec_millis() as f64);
                            }
                        }
                    }
                }
                Some(message::Union::cursor_data(_)) | Some(message::Union::cursor_position(_)) => {
                    cursors += 1;
                }
                _ => others += 1,
            },
            Err(e) => {
                if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut {
                    println!("(read timed out)");
                    break;
                }
                println!("recv error: {}", e);
                break;
            }
        }
    }

    let el = start.elapsed().as_secs() as f64 + start.elapsed().subsec_millis() as f64 / 1000.0;
    println!("\n--- result ---");
    println!("  video frames   {} ({} key), {} bytes total", frames, keyframes, bytes);
    if let Some(ms) = first_frame_ms {
        println!("  first frame    {:.0} ms after login", ms);
    }
    if frames > 0 {
        println!("  average        {} bytes/frame, {:.2} fps over {:.1} s",
                 bytes / frames as usize, frames as f64 / el.max(0.001), el);
    }
    println!("  cursor msgs    {}", cursors);
    println!("  other msgs     {}", others);
    println!("{}", if frames > 0 {
        "VERDICT: the agent is serving video to a peer."
    } else {
        "VERDICT: logged in but no video arrived."
    });
}
