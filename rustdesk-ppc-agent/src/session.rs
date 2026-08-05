//! One peer connection, start to finish: key exchange, login, message loop.
//!
//! Mirrors the agent side of `src/server.rs:90-135` (key exchange) and
//! `src/server/connection.rs` (login + dispatch), on blocking I/O.
//!
//! Two modes, matching upstream. Which one applies is decided by the *peer's*
//! connection route, not by us:
//!
//! **Direct IP (the default).** Upstream's `direct_server` calls
//! `create_tcp_connection(.., secure = false)`, and the whole signed_id/public_key
//! exchange in `server.rs` is gated on `if secure && ..`. A client connecting by
//! IP never calls `secure_connection` at all — the direct branch of `_start`
//! returns the socket immediately. So it sends nothing and waits for `hash`:
//!
//! ```text
//!   agent -> peer   Message{ hash }             salt + per-connection challenge
//!   peer  -> agent  Message{ login_request }    sha256(sha256(pw|salt)|challenge)
//!   agent -> peer   Message{ login_response }   peer_info, or an error string
//!   ...             mouse_event / key_event in, video_frame out
//! ```
//!
//! Sending `signed_id` here and waiting for `public_key` deadlocks: we wait for a
//! key exchange the peer will never start, it waits for a `hash` we never send.
//!
//! **Secure (`--secure`).** For a peer that knows our public key out of band:
//!
//! ```text
//!   agent -> peer   Message{ signed_id }        plaintext
//!   peer  -> agent  Message{ public_key }       plaintext; seals the secretbox key
//!   -- everything past here is secretbox-sealed --
//!   ... as above
//! ```

use std::io;
use std::net::TcpStream;

use protobuf::Message as _;

use crate::crypto::{expected_login_hash, verify_login_hash, Handshake, SecureChannel};
use crate::frame::{read_frame, write_frame, DEFAULT_MAX_PACKET};
use crate::message_proto::*;

/// Login error strings the client matches on **exactly** -- `handle_login_error`
/// in client.rs compares against these literals to decide which dialog to show,
/// so any deviation turns a password prompt into a silent retry loop.
/// Values from hbb_common/src/lib.rs.
mod login_msg {
    /// Client clears its stored password and shows the "Password Required" input.
    pub const PASSWORD_EMPTY: &str = "Empty Password";
    /// Client shows "Wrong Password -- do you want to enter again?".
    pub const PASSWORD_WRONG: &str = "Wrong Password";
    /// Password login is not permitted at all.
    pub const NO_PASSWORD_ACCESS: &str = "No Password Access";
}

/// Socket poll interval. Short enough that input stays responsive, long enough
/// that an idle screen costs little more than the dirty-band probe.
const POLL_MS: u64 = 30;
/// Modest by modern standards, but the encoder is not the constraint here.
const DEFAULT_BITRATE_KBPS: u32 = 1500;
/// Login attempts allowed on a single connection before dropping it. The peer
/// legitimately needs at least two (an empty probe, then the real password).
const MAX_LOGIN_ATTEMPTS: u32 = 10;

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
    /// Do the signed_id/public_key exchange and seal the session. Off by
    /// default: a direct-IP client does not participate, and enabling it there
    /// deadlocks the handshake.
    pub secure: bool,
}

/// Run one connection to completion. Errors are per-connection: the caller logs
/// and moves on to the next peer.
pub fn serve(stream: TcpStream, ident: &Identity) -> io::Result<()> {
    stream.set_nodelay(true).ok();
    let mut peer = Peer { stream, chan: None, name: String::new(), id: String::new() };

    log::info!("session start: {}", peer.stream.peer_addr().map(|a| a.to_string()).unwrap_or_default());

    // --- 1/2. key exchange (only when the peer will take part) ---------------
    if ident.secure {
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
                    // Upstream reads this as "no key for you, carry on in the
                    // clear". Honour it rather than dropping the peer.
                    log::warn!("step 2: peer declined encryption -- continuing UNENCRYPTED");
                } else {
                    let key = hs
                        .open_symmetric_key(&pk.asymmetric_value, &pk.symmetric_value)
                        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
                    peer.chan = Some(SecureChannel::new(key));
                    log::debug!("step 2: secure channel established (later frames sealed)");
                }
            }
            None => {
                // An empty Message is what `secure_connection` sends when it has
                // no public key for us.
                log::warn!("step 2: peer sent an empty message -- continuing UNENCRYPTED");
            }
            other => {
                log::error!(
                    "step 2: expected public_key, got {}",
                    msg_name(&Message { union: other, ..Default::default() })
                );
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "handshake: expected public_key",
                ));
            }
        }
    } else {
        log::info!("direct-IP mode: no key exchange, session is UNENCRYPTED (as upstream's direct_server)");
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
    // The peer may attempt several times on ONE connection: it probes with an
    // empty password, we ask it to prompt, and the user's answer arrives as a
    // second login_request on the same socket. Closing after the error is what
    // makes the client's password dialog flash up and vanish.
    let mut attempts = 0u32;
    loop {
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
                ));
            }
        };
        peer.name = lr.my_name.clone();
        peer.id = lr.my_id.clone();
        log::debug!(
            "step 4: login_request from '{}' (id '{}'), password {} bytes",
            peer.name, peer.id, lr.password.len()
        );

        if ident.password.is_empty() {
            // Nothing to authenticate against. Upstream would fall through to
            // the connection manager for interactive approval; we have no UI.
            peer.send_login_error(login_msg::NO_PASSWORD_ACCESS)?;
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "no password configured on this machine",
            ));
        }

        attempts += 1;
        if attempts > MAX_LOGIN_ATTEMPTS {
            log::warn!("too many login attempts from {} -- dropping", peer.id);
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "too many login attempts",
            ));
        }

        if lr.password.is_empty() {
            // A client always probes with an empty password first to discover
            // whether one is needed. PASSWORD_EMPTY is what makes it show the
            // dialog; the answer comes back on this same connection.
            log::info!("empty password -- asking the peer to prompt (attempt {})", attempts);
            peer.send_login_error(login_msg::PASSWORD_EMPTY)?;
            continue;
        }

        let expected =
            expected_login_hash(&ident.password, ident.salt.as_bytes(), challenge.as_bytes());
        if verify_login_hash(&expected, &lr.password) {
            break;
        }
        log::warn!("wrong password from {} ({}) -- attempt {}", peer.name, peer.id, attempts);
        peer.send_login_error(login_msg::PASSWORD_WRONG)?;
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
    /// Kept so the encoder can be rebuilt if the resolution changes.
    bitrate_kbps: u32,
    /// Set when the encoder could not be rebuilt; the session carries on with
    /// input only rather than dropping the peer.
    broken: bool,
}

#[cfg(all(target_os = "macos", not(no_vpx)))]
impl Video {
    fn new(bitrate_kbps: u32) -> Result<Self, &'static str> {
        let mut cap = crate::capture::Capturer::new()?;
        let img = crate::convert::I420::new(cap.width, cap.height);
        let enc = crate::encode::Encoder::new(img.width, img.height, bitrate_kbps)?;
        cap.invalidate();   // the new peer needs a full frame regardless
        Ok(Self {
            cap,
            enc,
            img,
            start: std::time::Instant::now(),
            bitrate_kbps,
            broken: false,
        })
    }

    /// Rebuild the conversion and encode buffers around the current geometry.
    fn resize(&mut self) -> Result<(), &'static str> {
        self.img = crate::convert::I420::new(self.cap.width, self.cap.height);
        self.enc = crate::encode::Encoder::new(self.img.width, self.img.height, self.bitrate_kbps)?;
        self.cap.invalidate();
        Ok(())
    }

    /// Probe, and if anything moved, capture/convert/encode one frame.
    /// Returns None when the screen is unchanged -- the cheap path, ~15 ms.
    fn next_frame(&mut self) -> Option<(Vec<u8>, bool, i64)> {
        // Before anything reads the framebuffer: the copy length is derived from
        // the cached geometry, so a resolution change that goes unnoticed reads
        // past the end of the mapping.
        if self.cap.refresh() {
            log::info!("display is now {}x{}; rebuilding the encoder", self.cap.width, self.cap.height);
            if let Err(e) = self.resize() {
                log::error!("could not restart the encoder at the new size: {}", e);
                self.broken = true;
            }
        }
        if self.broken {
            return None;
        }
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

/// Send the pointer's shape.
///
/// The client cannot draw a pointer, or act on any position, until it has an
/// image: its `setCursorPosition` returns immediately while no image is bound.
#[cfg(target_os = "macos")]
fn send_cursor_data(peer: &mut Peer) -> io::Result<()> {
    let c = crate::cursor::arrow();
    let mut cd = CursorData::new();
    cd.id = crate::cursor::CURSOR_ID;
    cd.hotx = c.hotx;
    cd.hoty = c.hoty;
    cd.width = c.width;
    cd.height = c.height;
    // Not raw pixels: the client runs this through zstd. See `zstd_frame`.
    cd.colors = crate::zstd_frame::raw_frame(&c.rgba);
    let mut m = Message::new();
    m.set_cursor_data(cd);
    peer.send(&m)
}

/// Send where the pointer is, if it has moved since last time.
#[cfg(target_os = "macos")]
fn send_cursor_position(peer: &mut Peer, tracker: &mut crate::cursor::Tracker) -> io::Result<()> {
    let (x, y) = crate::input::cursor_position();
    if x < 0.0 {
        return Ok(()); // the position was unreadable; nothing useful to send
    }
    if let Some((x, y)) = tracker.update(x as i32, y as i32) {
        let mut cp = CursorPosition::new();
        cp.x = x;
        cp.y = y;
        let mut m = Message::new();
        m.set_cursor_position(cp);
        peer.send(&m)?;
    }
    Ok(())
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
            // Worth spelling out: the usual cause is not the display at all but
            // a missing session. A fully detached agent cannot reach the window
            // server ("On-demand launch of the Window Server is allowed for root
            // user only"), and every display query then returns nonsense. The
            // session still runs, with input but no picture, which is a
            // confusing thing to debug from the client end.
            log::warn!("video unavailable: {} -- serving input only", e);
            log::warn!(
                "if the display reads 0x0, the agent has no window server: start it \
                 under `screen` or as the LaunchAgent, not with `&` or nohup"
            );
            None
        }
    };

    peer.stream.set_read_timeout(Some(std::time::Duration::from_millis(POLL_MS)))?;
    #[cfg(target_os = "macos")]
    let mut injector = crate::input::Injector::new();

    // The pointer is a hardware overlay and is not in the captured image, so
    // the peer sees none unless we send one. Shape once, then positions as they
    // change. See `crate::cursor`.
    #[cfg(target_os = "macos")]
    let mut cursor_tracker = crate::cursor::Tracker::new();
    #[cfg(target_os = "macos")]
    send_cursor_data(peer)?;

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

        // Also poll once an iteration: the person at the G5 can move the
        // pointer themselves, and no input event announces that.
        #[cfg(target_os = "macos")]
        send_cursor_position(peer, &mut cursor_tracker)?;

        // Drain everything already queued before spending another ~250 ms on a
        // frame. Handling one message per iteration was survivable when an idle
        // poll cost 6 ms; now that a capture cycle sets the floor, a client
        // sending 20-100 mouse events a second would outrun the loop and the
        // backlog would grow without bound -- the cursor would lag further
        // behind for as long as the session lasted.
        loop {
            let msg = match peer.recv() {
                Err(ref e) if is_timeout(e) => break,
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
                    {
                        injector.mouse(&me);
                        // Report the pointer here rather than once per video
                        // frame. A frame can take half a second, so polling
                        // alongside it made the peer's cursor jump between
                        // widely spaced positions instead of tracking.
                        send_cursor_position(peer, &mut cursor_tracker)?;
                    }
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
                // The peer's options. Worth logging rather than dropping: the
                // client only draws a remote pointer when its "show remote
                // cursor" toggle is on, and this is the only way to tell from
                // here whether it is -- upstream's server uses the same signal
                // to decide whether to send cursor data at all.
                Some(message::Union::misc(mi)) => {
                    if let Some(misc::Union::option(o)) = mi.union {
                        log::info!(
                            "peer options: show_remote_cursor={:?} image_quality={:?} \
                             disable_clipboard={:?} disable_audio={:?}",
                            o.show_remote_cursor.enum_value_or_default(),
                            o.image_quality.enum_value_or_default(),
                            o.disable_clipboard.enum_value_or_default(),
                            o.disable_audio.enum_value_or_default()
                        );
                        // The toggle is usually flipped mid-session, long after
                        // the shape was sent at login. Send it again, and make
                        // the next poll report a position unconditionally --
                        // otherwise the peer waits for the pointer to move
                        // before it can draw anything at all.
                        #[cfg(target_os = "macos")]
                        if o.show_remote_cursor.enum_value_or_default() == BoolOption::Yes {
                            send_cursor_data(peer)?;
                            cursor_tracker.reset();
                        }
                    }
                }
                other => log::debug!(
                    "   (no handler for {})",
                    msg_name(&Message { union: other, ..Default::default() })
                ),
            }
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
        run_login_inner(password, Some(peer_sends))
    }

    /// Send a raw password payload -- notably the empty one a client probes with.
    fn run_login_raw(password: &str, raw: Vec<u8>) -> Result<PeerInfo, String> {
        assert!(raw.is_empty(), "only the empty probe is exercised this way");
        run_login_inner(password, None)
    }

    fn run_login_inner(password: &str, peer_sends: Option<&str>) -> Result<PeerInfo, String> {
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
            secure: true,
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
        req.password = match peer_sends {
            Some(p) => expected_login_hash(p, hash.salt.as_bytes(), hash.challenge.as_bytes()),
            None => Vec::new(),
        };
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

    /// Direct-IP mode: the peer sends nothing until it gets a `hash`. This is
    /// the path a real RustDesk client takes when you type an IP, and getting it
    /// wrong deadlocks both sides.
    #[test]
    fn direct_ip_mode_sends_hash_first_without_key_exchange() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let (_pk, sk) = sign::gen_keypair();
        let ident = Identity {
            id: "123456789".into(),
            salt: "abcdef".into(),
            password: "hunter2".into(),
            secret_key: sk,
            hostname: "g5".into(),
            width: 800,
            height: 600,
            secure: false,
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
        // Send nothing, exactly as a direct-IP client does; the first thing we
        // must see is an unencrypted `hash`.
        let hash = match c.recv().unwrap().union {
            Some(message::Union::hash(h)) => h,
            other => panic!("expected hash first, got {:?}", other.is_some()),
        };
        assert_eq!(hash.salt, "abcdef");
        let mut req = LoginRequest::new();
        req.my_id = "peer".into();
        req.my_name = "tester".into();
        req.password =
            expected_login_hash("hunter2", hash.salt.as_bytes(), hash.challenge.as_bytes());
        let mut m = Message::new();
        m.set_login_request(req);
        c.send(&m).unwrap();
        match c.recv().unwrap().union {
            Some(message::Union::login_response(r)) => match r.union {
                Some(login_response::Union::peer_info(pi)) => assert_eq!(pi.hostname, "g5"),
                other => panic!("login failed: {:?}", other),
            },
            other => panic!("expected login_response, got {:?}", other.is_some()),
        }
        drop(c);
        let _ = t.join();
    }

    #[test]
    fn correct_password_completes_the_handshake() {
        let pi = run_login("hunter2", "hunter2").expect("login should succeed");
        assert_eq!(pi.hostname, "g5");
        assert_eq!(pi.displays.len(), 1);
        assert_eq!(pi.displays[0].width, 1024);
        assert_eq!(pi.displays[0].height, 768);
    }

    /// The real sequence: probe empty, get told to prompt, then send the
    /// password **on the same connection**. Closing after the error is what made
    /// the client's dialog flash up and vanish.
    #[test]
    fn retry_after_empty_password_succeeds_on_the_same_connection() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let (_pk, sk) = sign::gen_keypair();
        let ident = Identity {
            id: "1".into(),
            salt: "abcdef".into(),
            password: "hunter2".into(),
            secret_key: sk,
            hostname: "g5".into(),
            width: 800,
            height: 600,
            secure: false,
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
        let hash = match c.recv().unwrap().union {
            Some(message::Union::hash(h)) => h,
            other => panic!("expected hash, got {:?}", other.is_some()),
        };

        // 1. probe with an empty password
        let mut req = LoginRequest::new();
        req.my_name = "tester".into();
        let mut m = Message::new();
        m.set_login_request(req);
        c.send(&m).unwrap();
        match c.recv().unwrap().union {
            Some(message::Union::login_response(r)) => match r.union {
                Some(login_response::Union::error(e)) => assert_eq!(e, "Empty Password"),
                other => panic!("expected Empty Password, got {:?}", other),
            },
            other => panic!("expected login_response, got {:?}", other.is_some()),
        }

        // 2. the socket must still be open -- send the real password on it
        let mut req = LoginRequest::new();
        req.my_name = "tester".into();
        req.password =
            expected_login_hash("hunter2", hash.salt.as_bytes(), hash.challenge.as_bytes());
        let mut m = Message::new();
        m.set_login_request(req);
        c.send(&m).expect("connection must stay open after Empty Password");
        match c.recv().unwrap().union {
            Some(message::Union::login_response(r)) => match r.union {
                Some(login_response::Union::peer_info(pi)) => assert_eq!(pi.hostname, "g5"),
                other => panic!("retry should have succeeded, got {:?}", other),
            },
            other => panic!("expected login_response, got {:?}", other.is_some()),
        }
        drop(c);
        let _ = t.join();
    }

    /// A client probes with an empty password first. Answering "Wrong Password"
    /// there makes it loop instead of prompting, so pin the exact string.
    #[test]
    fn empty_password_asks_the_client_to_prompt() {
        let err = run_login_raw("hunter2", Vec::new()).unwrap_err();
        assert_eq!(err, "Empty Password");
    }

    #[test]
    fn wrong_password_is_refused() {
        let err = run_login("hunter2", "wrong").unwrap_err();
        assert_eq!(err, "Wrong Password");
    }

    #[test]
    fn unset_password_refuses_rather_than_allowing_anyone() {
        let err = run_login("", "anything").unwrap_err();
        assert_eq!(err, "No Password Access");
    }
}
