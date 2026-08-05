//! One peer connection, start to finish: key exchange, login, message loop.
//!
//! Mirrors the agent side of `src/server.rs:90-135` (key exchange) and
//! `src/server/connection.rs` (login + dispatch), on blocking I/O.
//!
//! Sequence, all frames length-prefixed by [`crate::frame`]:
//!
//! ```text
//!   agent -> peer   Message{ signed_id }        plaintext
//!   peer  -> agent  Message{ public_key }       plaintext; seals the secretbox key
//!   -- everything past here is secretbox-sealed --
//!   agent -> peer   Message{ hash }             salt + per-connection challenge
//!   peer  -> agent  Message{ login_request }    password = sha256(sha256(pw|salt)|challenge)
//!   agent -> peer   Message{ login_response }   peer_info, or an error string
//!   ...             mouse_event / key_event in, video_frame out
//! ```

use std::io;
use std::net::TcpStream;

use protobuf::Message as _;

use crate::crypto::{expected_login_hash, verify_login_hash, Handshake, SecureChannel};
use crate::frame::{read_frame, write_frame, DEFAULT_MAX_PACKET};
use crate::message_proto::*;

/// Socket poll interval. Short enough that input stays responsive, long enough
/// that an idle screen costs little more than the dirty-band probe.
const POLL_MS: u64 = 30;
/// Modest by modern standards, but the encoder is not the constraint here.
const DEFAULT_BITRATE_KBPS: u32 = 1500;

/// Human-readable name for a message, so a trace shows what a client actually
/// sent rather than "unhandled". Anything not listed is a field this vintage of
/// the proto does not know -- which is exactly what a modern client diverging
/// would look like.
fn msg_name(m: &Message) -> &'static str {
    match &m.union {
        Some(message::Union::signed_id(_)) => "signed_id",
        Some(message::Union::public_key(_)) => "public_key",
        Some(message::Union::test_delay(_)) => "test_delay",
        Some(message::Union::video_frame(_)) => "video_frame",
        Some(message::Union::login_request(_)) => "login_request",
        Some(message::Union::login_response(_)) => "login_response",
        Some(message::Union::hash(_)) => "hash",
        Some(message::Union::mouse_event(_)) => "mouse_event",
        Some(message::Union::audio_frame(_)) => "audio_frame",
        Some(message::Union::cursor_data(_)) => "cursor_data",
        Some(message::Union::cursor_position(_)) => "cursor_position",
        Some(message::Union::cursor_id(_)) => "cursor_id",
        Some(message::Union::key_event(_)) => "key_event",
        Some(message::Union::clipboard(_)) => "clipboard",
        Some(message::Union::file_action(_)) => "file_action",
        Some(message::Union::file_response(_)) => "file_response",
        Some(message::Union::misc(_)) => "misc",
        None => "<empty or unknown field>",
    }
}

/// A read timeout looks like WouldBlock or TimedOut depending on the platform.
fn is_timeout(e: &io::Error) -> bool {
    matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut)
}

/// First bytes of a buffer, for diagnosing a frame we could not make sense of.
fn hex_head(b: &[u8]) -> String {
    let n = b.len().min(24);
    let mut s = String::with_capacity(n * 3 + 8);
    for x in &b[..n] {
        s.push_str(&format!("{:02x} ", x));
    }
    if b.len() > n {
        s.push_str("...");
    }
    s
}

pub struct Peer {
    stream: TcpStream,
    chan: Option<SecureChannel>,
    pub name: String,
    pub id: String,
}

impl Peer {
    pub fn send(&mut self, msg: &Message) -> io::Result<()> {
        let body = msg
            .write_to_bytes()
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
        log::debug!("-> {} ({} bytes plain)", msg_name(msg), body.len());
        let body = match self.chan.as_mut() {
            Some(c) => c.seal(&body),
            None => body,
        };
        log::trace!("-> frame {} bytes on the wire", body.len());
        write_frame(&mut self.stream, &body)
    }

    pub fn recv(&mut self) -> io::Result<Message> {
        let raw = read_frame(&mut self.stream, DEFAULT_MAX_PACKET)?;
        log::trace!("<- frame {} bytes on the wire", raw.len());
        let body = match self.chan.as_mut() {
            Some(c) => c.open(&raw).map_err(|_| {
                log::error!(
                    "decryption failed on a {}-byte frame -- wrong key, or the peer \
                     is not speaking this protocol",
                    raw.len()
                );
                io::Error::new(io::ErrorKind::InvalidData, "decryption error")
            })?,
            None => raw,
        };
        match Message::parse_from_bytes(&body) {
            Ok(m) => {
                log::debug!("<- {} ({} bytes plain)", msg_name(&m), body.len());
                if m.union.is_none() {
                    // A field this proto vintage does not know: the shape a
                    // newer client's extra messages take.
                    log::warn!("   message had no recognised field; first bytes: {}", hex_head(&body));
                }
                Ok(m)
            }
            Err(e) => {
                log::error!("protobuf parse failed ({}); first bytes: {}", e, hex_head(&body));
                Err(io::Error::new(io::ErrorKind::InvalidData, e.to_string()))
            }
        }
    }

    fn send_login_error(&mut self, err: &str) -> io::Result<()> {
        let mut lr = LoginResponse::new();
        lr.set_error(err.to_owned());
        let mut m = Message::new();
        m.set_login_response(lr);
        self.send(&m)
    }
}

/// Everything the session needs from configuration, resolved once at startup so
/// the per-connection path does no I/O of its own.
pub struct Identity {
    pub id: String,
    pub salt: String,
    pub password: String,
    pub secret_key: sodiumoxide::crypto::sign::SecretKey,
    pub hostname: String,
    pub width: i32,
    pub height: i32,
}

/// Run one connection to completion. Errors are per-connection: the caller logs
/// and moves on to the next peer.
pub fn serve(stream: TcpStream, ident: &Identity) -> io::Result<()> {
    stream.set_nodelay(true).ok();
    let mut peer = Peer { stream, chan: None, name: String::new(), id: String::new() };

    log::info!("session start: {}", peer.stream.peer_addr().map(|a| a.to_string()).unwrap_or_default());

    // --- 1/2. key exchange ---------------------------------------------------
    log::debug!("step 1: sending signed_id (plaintext)");
    let hs = Handshake::new();
    let mut m = Message::new();
    let mut sid = SignedId::new();
    sid.id = hs.signed_id(&ident.id, &ident.secret_key);
    m.set_signed_id(sid);
    peer.send(&m)?;

    log::debug!("step 2: awaiting public_key");
    let reply = peer.recv()?;
    match reply.union {
        Some(message::Union::public_key(pk)) => {
            if pk.asymmetric_value.is_empty() {
                // Upstream reads this as "peer has no key for us, resend"; for a
                // direct-IP agent an unencrypted session is not worth supporting.
                return Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "peer offered no public key (unencrypted session refused)",
                ));
            }
            let key = hs
                .open_symmetric_key(&pk.asymmetric_value, &pk.symmetric_value)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
            peer.chan = Some(SecureChannel::new(key));
            log::debug!("step 2: secure channel established (all later frames sealed)");
        }
        other => {
            log::error!(
                "step 2: expected public_key, got {}",
                msg_name(&Message { union: other.clone(), ..Default::default() })
            );
            let _ = other;
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "handshake: expected public_key",
            ))
        }
    }

    // --- 3. challenge --------------------------------------------------------
    log::debug!("step 3: sending hash (salt + challenge)");
    let challenge = crate::config::Config::random_string(6);
    let mut m = Message::new();
    let mut hash = Hash::new();
    hash.salt = ident.salt.clone();
    hash.challenge = challenge.clone();
    m.set_hash(hash);
    peer.send(&m)?;

    // --- 4/5. login ----------------------------------------------------------
    log::debug!("step 4: awaiting login_request");
    let lr = match peer.recv()?.union {
        Some(message::Union::login_request(lr)) => lr,
        other => {
            log::error!(
                "step 4: expected login_request, got {}",
                msg_name(&Message { union: other, ..Default::default() })
            );
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "expected login_request",
            ))
        }
    };
    peer.name = lr.my_name.clone();
    peer.id = lr.my_id.clone();
    log::debug!(
        "step 4: login_request from '{}' (id '{}'), password {} bytes",
        peer.name, peer.id, lr.password.len()
    );

    if ident.password.is_empty() {
        peer.send_login_error("This machine has no password set")?;
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "no password configured",
        ));
    }
    let expected = expected_login_hash(&ident.password, ident.salt.as_bytes(), challenge.as_bytes());
    if !verify_login_hash(&expected, &lr.password) {
        peer.send_login_error("Wrong Password")?;
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("wrong password from {} ({})", peer.name, peer.id),
        ));
    }

    let mut pi = PeerInfo::new();
    pi.hostname = ident.hostname.clone();
    pi.platform = "Mac OS".to_owned();
    pi.version = env!("CARGO_PKG_VERSION").to_owned();
    let mut d = DisplayInfo::new();
    d.width = ident.width;
    d.height = ident.height;
    d.name = "Display".to_owned();
    d.online = true;
    pi.displays.push(d);
    let mut resp = LoginResponse::new();
    resp.set_peer_info(pi);
    let mut m = Message::new();
    m.set_login_response(resp);
    peer.send(&m)?;
    log::info!("peer '{}' ({}) logged in -- entering message loop", peer.name, peer.id);

    message_loop(&mut peer)
}

/// Video pump state, kept beside the message loop.
#[cfg(all(target_os = "macos", not(no_vpx)))]
struct Video {
    cap: crate::capture::Capturer,
    enc: crate::encode::Encoder,
    img: crate::convert::I420,
    start: std::time::Instant,
}

#[cfg(all(target_os = "macos", not(no_vpx)))]
impl Video {
    fn new(bitrate_kbps: u32) -> Result<Self, &'static str> {
        let mut cap = crate::capture::Capturer::new()?;
        let img = crate::convert::I420::new(cap.width, cap.height);
        let enc = crate::encode::Encoder::new(img.width, img.height, bitrate_kbps)?;
        cap.invalidate();   // the new peer needs a full frame regardless
        Ok(Self { cap, enc, img, start: std::time::Instant::now() })
    }

    /// Probe, and if anything moved, capture/convert/encode one frame.
    /// Returns None when the screen is unchanged -- the cheap path, ~6 ms.
    fn next_frame(&mut self) -> Option<(Vec<u8>, bool, i64)> {
        let dirty = self.cap.dirty_bands();
        if !dirty.iter().any(|d| *d) {
            return None;
        }
        let all = dirty.iter().all(|d| *d);
        let stride = self.cap.stride();
        let frame = self.cap.read_bands(&dirty);
        crate::convert::argb_to_i420(frame, stride, &mut self.img);
        let pts = self.start.elapsed().as_millis() as i64;
        match self.enc.encode(&self.img, pts, all) {
            Ok(f) if !f.data.is_empty() => Some((f.data.to_vec(), f.key, f.pts_ms)),
            Ok(_) => None,
            Err(e) => {
                log::warn!("encode failed: {}", e);
                None
            }
        }
    }
}

/// Dispatch until the peer disconnects, pumping video between messages.
///
/// Single-threaded on purpose: one peer, one capture, and the encode is the
/// expensive step. The socket is polled with a short timeout so an idle screen
/// costs only the ~6 ms dirty-band probe.
fn message_loop(peer: &mut Peer) -> io::Result<()> {
    #[cfg(all(target_os = "macos", not(no_vpx)))]
    let mut video = match Video::new(DEFAULT_BITRATE_KBPS) {
        Ok(v) => Some(v),
        Err(e) => {
            log::warn!("video unavailable: {} (input-only session)", e);
            None
        }
    };

    peer.stream.set_read_timeout(Some(std::time::Duration::from_millis(POLL_MS)))?;
    #[cfg(target_os = "macos")]
    let mut injector = crate::input::Injector::new();

    loop {
        // Pump one video frame, if the screen moved.
        #[cfg(all(target_os = "macos", not(no_vpx)))]
        if let Some(v) = video.as_mut() {
            if let Some((data, key, pts)) = v.next_frame() {
                let mut vp = crate::message_proto::VP9::new();
                vp.data = data;
                vp.key = key;
                vp.pts = pts;
                let mut vp9s = crate::message_proto::VP9s::new();
                vp9s.frames.push(vp);
                let mut vf = crate::message_proto::VideoFrame::new();
                // Field 12, not 6: we encode VP8, and a modern client feeds
                // field 6 (`vp9s`) to its VP9 decoder.
                vf.set_vp8s(vp9s);
                let mut m = Message::new();
                m.set_video_frame(vf);
                peer.send(&m)?;
            }
        }

        let msg = match peer.recv() {
            Err(ref e) if is_timeout(e) => continue,
            Ok(m) => m,
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                log::info!("peer {} disconnected", peer.name);
                return Ok(());
            }
            Err(e) => return Err(e),
        };
        match msg.union {
            Some(message::Union::mouse_event(me)) => {
                #[cfg(target_os = "macos")]
                injector.mouse(&me);
                let _ = &me;
            }
            Some(message::Union::key_event(ke)) => {
                #[cfg(target_os = "macos")]
                injector.key(&ke);
                let _ = &ke;
            }
            Some(message::Union::test_delay(t)) => {
                if t.from_client {
                    let mut m = Message::new();
                    m.set_test_delay(t);
                    peer.send(&m)?;
                }
            }
            Some(message::Union::misc(_)) => {}
            other => log::debug!(
                "   (no handler for {})",
                msg_name(&Message { union: other, ..Default::default() })
            ),
        }
    }
}

/// Accept peers forever, one at a time.
///
/// Single-session on purpose: the G5 can barely encode for one viewer, and a
/// second concurrent capture would make both unusable.
pub fn listen(addr: &str, ident: &Identity) -> io::Result<()> {
    let listener = std::net::TcpListener::bind(addr)?;
    log::info!("agent listening on {} (id {})", addr, ident.id);
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let who = s.peer_addr().map(|a| a.to_string()).unwrap_or_default();
                log::info!("connection from {}", who);
                if let Err(e) = serve(s, ident) {
                    log::warn!("session with {} ended: {}", who, e);
                }
            }
            Err(e) => log::warn!("accept failed: {}", e),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use sodiumoxide::crypto::{box_, secretbox, sign};

    /// Drive the agent's handshake from the peer side, exactly as client.rs does,
    /// and check a correct password is accepted and a wrong one refused.
    fn run_login(password: &str, peer_sends: &str) -> Result<PeerInfo, String> {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let (pk, sk) = sign::gen_keypair();
        let ident = Identity {
            id: "123456789".into(),
            salt: "abcdef".into(),
            password: password.into(),
            secret_key: sk,
            hostname: "g5".into(),
            width: 1024,
            height: 768,
        };
        let t = std::thread::spawn(move || {
            let (s, _) = listener.accept().unwrap();
            let _ = serve(s, &ident);
        });

        let mut c = Peer {
            stream: TcpStream::connect(addr).unwrap(),
            chan: None,
            name: String::new(),
            id: String::new(),
        };

        // 1. SignedId -> verify with the agent's public key, take its ephemeral pk
        let signed = match c.recv().unwrap().union {
            Some(message::Union::signed_id(s)) => s.id,
            _ => return Err("no signed_id".into()),
        };
        let opened = sign::verify(&signed, &pk).map_err(|_| "bad signature")?;
        let text = String::from_utf8(opened).unwrap();
        let their_pk_b64 = text.splitn(2, '\0').nth(1).unwrap().to_owned();
        let their_pk_raw = base64_decode(&their_pk_b64).ok_or("bad b64")?;
        let mut pkb = [0u8; box_::PUBLICKEYBYTES];
        pkb.copy_from_slice(&their_pk_raw);

        // 2. seal a fresh symmetric key to it (zero nonce, as upstream does)
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
        c.send(&m).unwrap();
        c.chan = Some(SecureChannel::new(sym));

        // 3. Hash
        let hash = match c.recv().unwrap().union {
            Some(message::Union::hash(h)) => h,
            _ => return Err("no hash".into()),
        };

        // 4. LoginRequest
        let mut req = LoginRequest::new();
        req.my_id = "peer".into();
        req.my_name = "tester".into();
        req.password = expected_login_hash(
            peer_sends,
            hash.salt.as_bytes(),
            hash.challenge.as_bytes(),
        );
        let mut m = Message::new();
        m.set_login_request(req);
        c.send(&m).unwrap();

        // 5. LoginResponse
        let out = match c.recv().unwrap().union {
            Some(message::Union::login_response(r)) => match r.union {
                Some(login_response::Union::peer_info(pi)) => Ok(pi),
                Some(login_response::Union::error(e)) => Err(e),
                None => Err("empty login_response".into()),
            },
            _ => Err("no login_response".into()),
        };
        drop(c);
        let _ = t.join();
        out
    }

    fn base64_decode(s: &str) -> Option<Vec<u8>> {
        const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let mut out = Vec::new();
        let mut acc = 0u32;
        let mut bits = 0;
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

    #[test]
    fn correct_password_completes_the_handshake() {
        let pi = run_login("hunter2", "hunter2").expect("login should succeed");
        assert_eq!(pi.hostname, "g5");
        assert_eq!(pi.displays.len(), 1);
        assert_eq!(pi.displays[0].width, 1024);
        assert_eq!(pi.displays[0].height, 768);
    }

    #[test]
    fn wrong_password_is_refused() {
        let err = run_login("hunter2", "wrong").unwrap_err();
        assert_eq!(err, "Wrong Password");
    }

    #[test]
    fn unset_password_refuses_rather_than_allowing_anyone() {
        let err = run_login("", "anything").unwrap_err();
        assert!(err.contains("no password"), "got {:?}", err);
    }
}
