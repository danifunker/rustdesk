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

use rustdesk_ppc_agent::convert::I420;
use rustdesk_ppc_agent::crypto::{expected_login_hash, SecureChannel};
use rustdesk_ppc_agent::encode::Decoder;
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
    // Optional: `move X Y`, sent once after login. The point is the round trip
    // rather than the click -- on a downscaled session the peer's coordinates
    // are in the *served* space, and the position the agent reports back comes
    // through the same transform in the other direction. Ask for (100, 100) on
    // a 1/2 session, get (100, 100) back, and both halves of the scaling agree.
    // `decode` turns on decoding every frame and writing the last one out.
    //
    // Off by default, and that is not a preference. The guest is a single
    // processor: decoding costs it roughly what encoding costs it, and running
    // both in the same machine measured 0.67 fps against 1.21 for the same
    // agent and the same screen. A throughput number taken with the decoder
    // running is a number about the decoder.
    let want_decode = args.iter().any(|a| a == "decode");
    // `spin` drives the pointer in a circle for the whole session, one move
    // every 120 ms.
    //
    // This is the measurement the fixed workloads could not give. A screen that
    // changes once a second reports one frame a second however fast the agent
    // is -- the frame rate is the rate of change, not the capability -- and a
    // scrolling xterm goes to the other extreme and repaints its whole window.
    // A moving pointer is small, continuous, and exactly what a person does
    // most of: the cursor is composited into the capture on this platform, so
    // moving it dirties a few macroblocks and nothing else.
    // `spin [ms]` -- default 40 ms, i.e. 25 moves a second, which is faster
    // than the agent can serve. That is deliberate: at one move per 120 ms the
    // agent kept up with every single one and the measurement described the
    // *peer*, not the agent.
    let spin_ms: Option<u64> = args.iter().position(|a| a == "spin").map(|i| {
        args.get(i + 1).and_then(|v| v.parse().ok()).unwrap_or(40)
    });
    let spin = spin_ms.is_some();
    // `quality low|balanced|best`, sent as an OptionMessage after login. The
    // agent turns it into a downscale and answers with a SwitchDisplay, so this
    // is the only way to check the resolution dial the peer is actually holding.
    let quality: Option<ImageQuality> = args
        .iter()
        .position(|a| a == "quality")
        .and_then(|i| args.get(i + 1))
        .and_then(|q| match q.as_str() {
            "low" => Some(ImageQuality::Low),
            "balanced" => Some(ImageQuality::Balanced),
            "best" => Some(ImageQuality::Best),
            _ => None,
        });
    let mouse_at: Option<(i32, i32)> = args.iter().position(|a| a == "move").and_then(|i| {
        let x = args.get(i + 1)?.parse().ok()?;
        let y = args.get(i + 2)?.parse().ok()?;
        Some((x, y))
    });

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
            finish_login(&mut c, &hash, &password, secs, mouse_at, want_decode, quality, spin_ms);
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

    finish_login(&mut c, &hash, &password, secs, mouse_at, want_decode, quality, spin_ms);
}

fn finish_login(
    c: &mut Peer,
    hash: &Hash,
    password: &str,
    secs: u64,
    mouse_at: Option<(i32, i32)>,
    want_decode: bool,
    quality: Option<ImageQuality>,
    spin_ms: Option<u64>,
) {
    let spin = spin_ms.is_some();
    let spin_gap = Duration::from_millis(spin_ms.unwrap_or(40).max(10));
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

    // Ask for a picture size. The agent answers with a SwitchDisplay carrying
    // the size it will now send, which the loop below prints.
    if let Some(q) = quality {
        let mut o = OptionMessage::new();
        o.image_quality = ::protobuf::ProtobufEnumOrUnknown::new(q);
        let mut mi = Misc::new();
        mi.set_option(o);
        let mut m = Message::new();
        m.set_misc(mi);
        match c.send(&m) {
            Ok(_) => println!("asked for image_quality {:?}", q),
            Err(e) => println!("option send failed: {}", e),
        }
    }

    // Optionally drive the pointer, so the coordinate scaling can be checked
    // against a running agent rather than only in a unit test.
    if let Some((x, y)) = mouse_at {
        let mut me = MouseEvent::new();
        me.mask = 0;   // kind 0: a plain absolute move
        me.x = x;
        me.y = y;
        let mut m = Message::new();
        m.set_mouse_event(me);
        match c.send(&m) {
            Ok(_) => println!("sent a pointer move to ({}, {}) in the served space", x, y),
            Err(e) => println!("mouse send failed: {}", e),
        }
    }

    // Decode what arrives. The frames have to go through in order and none may
    // be skipped -- an inter frame is a difference against its predecessor --
    // so this decodes every one and keeps the last, which is then written out
    // as a PPM anyone can look at. That is the only check that covers the whole
    // chain: capture, byte order, downscale, colour conversion, encode, decode.
    let mut dec = if !want_decode {
        None
    } else {
        match Decoder::new() {
            Ok(d) => Some(d),
            Err(e) => {
                println!("no decoder ({}); frames will be counted but not checked", e);
                None
            }
        }
    };
    let mut pic = I420::new(16, 16);
    let mut decoded = 0u32;
    let mut decode_errs = 0u32;

    // 6. Watch the video pump.
    println!("\nwatching for {} s ...", secs);
    let start = Instant::now();
    let (mut frames, mut bytes, mut keyframes, mut cursors, mut others) = (0u32, 0usize, 0u32, 0u32, 0u32);
    let mut first_frame_ms = None;
    // Short reads when driving the pointer, so a move goes out on schedule
    // rather than whenever a frame happens to arrive.
    let _ = c.stream.set_read_timeout(Some(if spin {
        spin_gap
    } else {
        Duration::from_secs(secs.max(5))
    }));
    let mut spun = 0u32;
    let mut last_spin = Instant::now();
    if spin {
        // IRIX has no SO_RCVTIMEO -- setsockopt returns ENOPROTOOPT and the
        // read timeout above is silently not in force. The first version of
        // this measurement therefore blocked in recv until a frame arrived and
        // sent exactly one pointer move per frame: 302 moves, 301 frames, and a
        // "frame rate" that was really the round trip of its own loop. The
        // agent hit the same trap in its message loop and answers it with
        // poll(2); so does this.
        println!("  (pacing with poll: this platform has no SO_RCVTIMEO)");
    }
    while start.elapsed() < Duration::from_secs(secs) {
        // Only read when there is something to read, so the pointer keeps its
        // own schedule instead of inheriting the agent's.
        #[cfg(target_os = "irix")]
        if spin && !rustdesk_ppc_agent::sys::wait_readable(&c.stream, 5) {
            if last_spin.elapsed() >= spin_gap {
                last_spin = Instant::now();
                let a = spun as f64 * 0.35;
                let mut me = MouseEvent::new();
                me.mask = 0;
                me.x = (320.0 + 120.0 * a.cos()) as i32;
                me.y = (256.0 + 120.0 * a.sin()) as i32;
                let mut m = Message::new();
                m.set_mouse_event(me);
                if c.send(&m).is_err() {
                    break;
                }
                spun += 1;
            }
            continue;
        }
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
                            if let Some(d) = dec.as_mut() {
                                match d.decode(&f.data, &mut pic) {
                                    Ok(_) => decoded += 1,
                                    Err(_) => decode_errs += 1,
                                }
                            }
                        }
                    }
                }
                Some(message::Union::cursor_position(cp)) => {
                    cursors += 1;
                    if mouse_at.is_some() && cursors < 8 {
                        println!("  agent reports the pointer at ({}, {})", cp.x, cp.y);
                    }
                }
                Some(message::Union::cursor_data(_)) => {
                    cursors += 1;
                }
                Some(message::Union::misc(mi)) => {
                    others += 1;
                    if let Some(misc::Union::switch_display(sd)) = mi.union {
                        println!("  agent switched the display to {}x{}", sd.width, sd.height);
                    }
                }
                _ => others += 1,
            },
            Err(e) => {
                if (e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut)
                    && spin
                {
                    // Expected: the short timeout is the pointer's clock.
                }
                else if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut {
                    println!("(read timed out)");
                    break;
                } else {
                println!("recv error: {}", e);
                break;
                }
            }
        }
        if spin && last_spin.elapsed() >= spin_gap {
            last_spin = Instant::now();
            // A circle of radius 120 about the middle of a 640x512 frame, in
            // the peer's own coordinate space -- the agent scales it back up.
            let a = spun as f64 * 0.35;
            let mut me = MouseEvent::new();
            me.mask = 0;
            me.x = (320.0 + 120.0 * a.cos()) as i32;
            me.y = (256.0 + 120.0 * a.sin()) as i32;
            let mut m = Message::new();
            m.set_mouse_event(me);
            if c.send(&m).is_err() {
                break;
            }
            spun += 1;
        }
    }
    if spin {
        println!("  pointer moves sent {} (one every {} ms)", spun, spin_gap.as_millis());
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
    if dec.is_some() {
        println!("  decoded        {} ok, {} refused", decoded, decode_errs);
        if decoded > 0 {
            write_ppm("/tmp/decoded.ppm", &pic);
        }
    }
    println!("{}", if frames > 0 {
        "VERDICT: the agent is serving video to a peer."
    } else {
        "VERDICT: logged in but no video arrived."
    });
}


/// Write an I420 picture out as a PPM, converting through the inverse of the
/// coefficients `convert.rs` uses.
///
/// PPM because it needs no library on either side and `od` can read a pixel out
/// of it over telnet, which is how this gets checked when there is no way to
/// display an image on the machine under test.
fn write_ppm(path: &str, img: &I420) {
    let (w, h) = (img.width, img.height);
    if w == 0 || h == 0 {
        return;
    }
    let cs = img.chroma_stride();
    let mut out = format!("P6\n{} {}\n255\n", w, h).into_bytes();
    out.reserve(w * h * 3);
    for y in 0..h {
        for x in 0..w {
            let yy = img.y[y * w + x] as i32 - 16;
            let u = img.u[(y / 2) * cs + x / 2] as i32 - 128;
            let v = img.v[(y / 2) * cs + x / 2] as i32 - 128;
            let c = |n: i32| if n < 0 { 0u8 } else if n > 255 { 255 } else { n as u8 };
            out.push(c((298 * yy + 409 * v + 128) >> 8));
            out.push(c((298 * yy - 100 * u - 208 * v + 128) >> 8));
            out.push(c((298 * yy + 516 * u + 128) >> 8));
        }
    }
    match std::fs::write(path, &out) {
        Ok(_) => println!("  wrote {} ({}x{}) -- the picture the peer actually got", path, w, h),
        Err(e) => println!("  could not write {}: {}", path, e),
    }
}
