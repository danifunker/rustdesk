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
/// How long the screen must be still before re-reading a slice of it to cover
/// anything the sampled change detection missed, and how much to re-read each
/// time.
///
/// **A slice, not the screen.** Repainting all sixteen bands at once costs a
/// ~370 ms stall, and ordinary typing pauses for longer than `SETTLE_REPAINT`
/// several times a minute -- so a real terminal session hit a full repaint
/// every ~1.4 seconds, each one freezing the picture for 0.6-1.5 s while the
/// typing itself cost only 110 ms. Insurance that expensive was worse than what
/// it insured against. Two bands is ~45 ms, and the rotation covers the whole
/// screen within about seven ticks of quiet.
const SETTLE_REPAINT: std::time::Duration = std::time::Duration::from_millis(900);
const REPAIR_BANDS_PER_TICK: usize = crate::capture::BANDS / 8;

/// How long the screen must be still before the probe backs off, and how far
/// apart probes may then be.
///
/// The probe is not free -- it reads VRAM, which is the slow thing on this
/// machine -- and running it flat out on a desktop that has not moved for
/// several seconds spends a processor the person sitting at the G5 might want.
/// The cost is latency on the *first* change after a quiet spell, bounded by
/// the interval; every change after that is noticed at full rate, because any
/// dirty band resets the clock. Deliberately shorter than `SETTLE_REPAINT`'s
/// window so the repaint that covers missed changes is never delayed by it.
const IDLE_AFTER: std::time::Duration = std::time::Duration::from_millis(3000);
const IDLE_PROBE_INTERVAL: std::time::Duration = std::time::Duration::from_millis(150);

/// How often to send the peer a `TestDelay`, and how long to wait for an answer
/// before sending another anyway.
///
/// **This is what keeps the session alive, not a diagnostic.** Liveness in this
/// protocol is driven by the *server*: upstream's `Connection::run` ticks every
/// three seconds, sends a `TestDelay`, and drops the peer if nothing has been
/// received for thirty (`src/server/connection.rs:281`). The client only ever
/// answers -- it never initiates one. An agent that merely echoes therefore
/// goes completely silent the moment the screen stops changing, and the client
/// times out and reconnects on its own.
///
/// Measured before this existed: after the settle repaint's last frame, not one
/// byte crossed the wire in either direction, and the client dropped the
/// session 57.4 seconds later. Three times running, to the tenth of a second.
/// Longest a peer goes without an exact frame. See the settle repaint.
const KEYFRAME_INTERVAL: std::time::Duration = std::time::Duration::from_secs(10);

const TEST_DELAY_INTERVAL: std::time::Duration = std::time::Duration::from_secs(3);
const TEST_DELAY_STALE: std::time::Duration = std::time::Duration::from_secs(10);

/// Login attempts allowed on a single connection before dropping it. The peer
/// legitimately needs at least two (an empty probe, then the real password).
const MAX_LOGIN_ATTEMPTS: u32 = 10;

/// The version reported to the peer, and **not** the crate's own.
///
/// A modern client changes what it *sends* based on what we claim to be, so
/// this is a capability declaration rather than a label. **Raise it only for
/// behaviour that is implemented and has been tried against a real client.**
/// The gates, from the current client's `src/common.rs`:
///
/// * **1.2.0** -- at or above it the client's default keyboard mode becomes
///   `Map`, where `chr` carries a keycode already translated for this platform
///   instead of a character. *Implemented and verified*: `decide_key` reads
///   `KeyEvent.mode`, and typing was confirmed in both modes against a real
///   client. Hence the value below.
/// * **1.2.4** -- the refresh button switches to `Misc::refresh_video_display`
///   (field 31) from `refresh_video` (field 10). Backported and handled, but the
///   rest of what this version gates has not been surveyed. **Not yet.**
/// * **1.4.5** -- the client may use *relative* mouse mode and send deltas
///   rather than absolute coordinates. `decide_mouse` takes absolutes, so the
///   pointer would come apart entirely. **Not yet.**
///
/// It used to be `env!("CARGO_PKG_VERSION")`, which happened to be 0.1.0 and
/// happened to be safe. Bumping a crate version is an ordinary thing to do and
/// would silently have moved the client onto paths this agent does not
/// implement, with no error anywhere. Pinned here, and guarded by a test that
/// encodes exactly which gates have been earned.
const REPORTED_VERSION: &str = "1.2.0";

/// Is there anything to read without waiting?
///
/// `drain_input` blocks for `POLL_MS` whenever the socket is empty. At the top
/// of the loop that is exactly right -- it is what paces an idle session. After
/// every band it is exactly wrong, and the instrumentation put a number on it:
/// a sixteen-band frame paid sixteen of those timeouts, 481 ms of a 1208 ms
/// frame spent asleep on an empty socket.
///
/// Peeking consumes nothing, so the framing `read_frame` depends on is
/// untouched -- which a shorter read timeout would not be, since a timeout
/// landing mid-message leaves the stream out of sync with no way back.
fn input_waiting(stream: &TcpStream) -> bool {
    let mut b = [0u8; 1];
    if stream.set_nonblocking(true).is_err() {
        return true; // cannot tell; fall back to the blocking drain
    }
    let r = stream.peek(&mut b);
    let _ = stream.set_nonblocking(false);
    match r {
        // Data waiting, or 0 bytes meaning the peer has gone -- either way the
        // real read should run and deal with it.
        Ok(_) => true,
        Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => false,
        Err(_) => true,
    }
}

/// Milliseconds since the epoch, as `TestDelay.time` carries.
///
/// Only ever subtracted from another reading of this same clock, so a wrong
/// wall clock costs nothing -- and a G5 that has been off for a while often has
/// one.
fn now_millis() -> i64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => d.as_millis() as i64,
        Err(_) => 0,
    }
}

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
        Some(message::Union::pointer_device_event(_)) => "pointer_device_event",
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
    pi.version = REPORTED_VERSION.to_owned();
    let mut d = DisplayInfo::new();
    // The size now, not the size when the process started -- the resolution may
    // have been changed since, and this is what sizes the peer's canvas.
    #[cfg(target_os = "macos")]
    let (dw, dh) = crate::capture::display_size().unwrap_or((ident.width, ident.height));
    #[cfg(not(target_os = "macos"))]
    let (dw, dh) = (ident.width, ident.height);
    d.width = dw;
    d.height = dh;
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
    /// Set when the colour depth left 32, so the warning is logged once.
    bpp_warned: bool,
    /// When the screen last changed. See `probe`.
    last_change: std::time::Instant,
    /// Bands still to be re-read since the last detected change, and where the
    /// rotation is up to. Reset to a full screen's worth whenever anything
    /// moves; drained `REPAIR_BANDS_PER_TICK` at a time while it stays still.
    repair_left: usize,
    repair_next: usize,
    last_repair: std::time::Instant,
    /// When the framebuffer was last sampled, for the idle backoff.
    last_probe: std::time::Instant,
    /// Where the current frame's time is going. See `FrameTimes`.
    t: FrameTimes,
    /// Send the next frame as a keyframe.
    ///
    /// Deliberately *not* "lots of the screen changed". That was the rule until
    /// live instrumentation showed what it cost: dragging a window dirties all
    /// sixteen bands, so the worst case for capture was also forced to be the
    /// worst case for the encoder -- 170-181 ms and ~95 KB, against 27-78 ms
    /// and a few KB for the inter frame that would have done. Nothing needed
    /// it. A fresh encoder emits a keyframe by itself, so a new peer and a
    /// resize are covered; `kf_mode` is `VPX_KF_AUTO`, so libvpx still places
    /// them where they pay; and the stream is TCP, so an inter frame cannot
    /// arrive without its predecessor. What is left is the peer *asking*, which
    /// it can now do -- see `Misc::refresh_video`.
    want_key: bool,
    /// When the last keyframe went out, so the settle repaint can pace them.
    last_key: std::time::Instant,
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
            bpp_warned: false,
            last_change: std::time::Instant::now(),
            repair_left: 0,
            repair_next: 0,
            last_repair: std::time::Instant::now(),
            last_probe: std::time::Instant::now(),
            t: FrameTimes::default(),
            want_key: true,
            last_key: std::time::Instant::now(),
            broken: false,
        })
    }

    /// Rebuild the conversion and encode buffers around the current geometry.
    fn resize(&mut self) -> Result<(), &'static str> {
        self.img = crate::convert::I420::new(self.cap.width, self.cap.height);
        self.enc = crate::encode::Encoder::new(self.img.width, self.img.height, self.bitrate_kbps)?;
        self.cap.invalidate();
        self.want_key = true;
        Ok(())
    }

    /// Re-read the geometry, rebuilding around it if it moved.
    ///
    /// Must run before anything reads the framebuffer: the copy length comes
    /// from the cached geometry, so a resolution change that goes unnoticed
    /// reads past the end of the mapping. Returns the new size when it changed,
    /// so the caller can tell the peer *before* sending a frame in it.
    fn poll_geometry(&mut self) -> Option<(i32, i32)> {
        if !self.cap.refresh() {
            return None;
        }
        log::info!("display is now {}x{}; rebuilding the encoder", self.cap.width, self.cap.height);
        if let Err(e) = self.resize() {
            log::error!("could not restart the encoder at the new size: {}", e);
            self.broken = true;
        }
        Some((self.cap.width as i32, self.cap.height as i32))
    }

    /// Probe for change. Returns the dirty bands, or None if nothing moved --
    /// the cheap path, ~15 ms.
    fn probe(&mut self) -> Option<[bool; crate::capture::BANDS]> {
        if self.broken {
            return None;
        }
        // Nothing has moved for a while: sample at a slower cadence rather than
        // reading VRAM as fast as the loop comes round. Gated on `repaired` so
        // the settle repaint below always happens at full rate first, and any
        // dirty band puts it straight back to probing every pass.
        if self.repair_left == 0
            && self.last_change.elapsed() >= IDLE_AFTER
            && self.last_probe.elapsed() < IDLE_PROBE_INTERVAL
        {
            return None;
        }
        self.last_probe = std::time::Instant::now();
        // A 16-bit mode is reachable from the Displays pane, and the converter
        // assumes 32. Pause rather than send garbage, and pick up again by
        // itself if the depth comes back.
        let bpp = self.cap.bits_per_pixel();
        if bpp != 32 {
            if !self.bpp_warned {
                log::warn!("display is {} bits per pixel, not 32; video paused", bpp);
                self.bpp_warned = true;
            }
            return None;
        }
        if self.bpp_warned {
            log::info!("display is 32 bits per pixel again; resuming video");
            self.bpp_warned = false;
            self.cap.invalidate();
        }
        // Change detection is a sampled checksum, so it can miss: a small
        // change that falls between sampled rows and columns leaves the peer
        // showing stale pixels indefinitely, with nothing to correct it. Once
        // the screen has been still for a moment, re-read a slice of it, and
        // keep going a slice at a time until the whole screen has been covered.
        // Bounded staleness without the stall -- see SETTLE_REPAINT.
        //
        // Marked *before* the probe so that one probe serves both purposes.
        // Invalidating and then re-probing to discover the bands we had just
        // marked ourselves cost a second full pass: 81 ms where 40 would do, on
        // every tick of the rotation.
        let repairing = self.repair_left > 0
            && self.last_change.elapsed() >= SETTLE_REPAINT
            && self.last_repair.elapsed() >= SETTLE_REPAINT;
        if repairing {
            self.last_repair = std::time::Instant::now();
            let n = REPAIR_BANDS_PER_TICK.min(self.repair_left);
            for _ in 0..n {
                self.cap.invalidate_band(self.repair_next);
                self.repair_next = (self.repair_next + 1) % crate::capture::BANDS;
                self.repair_left -= 1;
            }
            log::debug!("repairing {} band(s); {} left to cover", n, self.repair_left);
            // The one keyframe sent on a schedule: the encoder's static-skip
            // threshold leaves a block uncoded while its error stays small, so
            // a faint difference is bounded but not self-correcting.
            if self.last_key.elapsed() >= KEYFRAME_INTERVAL {
                self.want_key = true;
            }
        }

        let tp = std::time::Instant::now();
        let dirty = self.cap.dirty_bands();
        if dirty.iter().any(|d| *d) {
            // A frame is starting: reset the accounting and charge it the probe
            // that found the change.
            self.t = FrameTimes::default();
            self.t.probe = tp.elapsed();
            self.last_change = std::time::Instant::now();
            // A repair tick dirties bands by design. Letting that restart the
            // rotation would mean it never finished a lap of the screen.
            if !repairing {
                self.repair_left = crate::capture::BANDS;
            }
            return Some(dirty);
        }
        None
    }

    /// Read one band out of VRAM and convert just those rows.
    ///
    /// Deliberately per-band: the caller services input between bands, so a
    /// screen-wide change no longer blocks input for the ~520 ms that reading
    /// and converting a whole frame takes.
    fn band(&mut self, b: usize) {
        let stride = self.cap.stride();
        let t = std::time::Instant::now();
        let (y0, y1) = self.cap.read_band(b);
        let read = t.elapsed();
        crate::convert::argb_to_i420_rows(self.cap.buffer(), stride, &mut self.img, y0, y1);
        self.t.convert += t.elapsed() - read;
        self.t.read += read;
        self.t.bands += 1;
    }

    /// Ask for the next frame to be a keyframe, and for everything to be
    /// re-read so it has current pixels to key on. What the peer's refresh
    /// button reaches.
    fn request_key(&mut self) {
        self.want_key = true;
        self.cap.invalidate();
    }

    /// Encode whatever is in the conversion buffer.
    fn encode(&mut self) -> Option<(Vec<u8>, bool, i64)> {
        let pts = self.start.elapsed().as_millis() as i64;
        let force = self.want_key;
        self.want_key = false;
        if force {
            self.last_key = std::time::Instant::now();
        }
        let t = std::time::Instant::now();
        let r = self.enc.encode(&self.img, pts, force);
        self.t.encode = t.elapsed();
        match r {
            Ok(f) if !f.data.is_empty() => {
                self.t.bytes = f.data.len();
                self.t.key = f.key;
                Some((f.data.to_vec(), f.key, f.pts_ms))
            }
            Ok(_) => None,
            Err(e) => {
                log::warn!("encode failed: {}", e);
                None
            }
        }
    }
}

/// Where one frame's milliseconds went.
///
/// Every stage is timed separately because the alternative -- inferring cost
/// from the gap between sends -- cannot tell work from a screen that simply was
/// not changing, and a live session is the only place some of this shows up at
/// all. `--probe-display` measures the same stages in isolation; this measures
/// them under a real client, with input arriving and a socket to write to.
#[cfg(all(target_os = "macos", not(no_vpx)))]
#[derive(Default)]
struct FrameTimes {
    probe: std::time::Duration,
    read: std::time::Duration,
    convert: std::time::Duration,
    encode: std::time::Duration,
    send: std::time::Duration,
    bands: usize,
    bytes: usize,
    key: bool,
}

#[cfg(all(target_os = "macos", not(no_vpx)))]
impl FrameTimes {
    fn ms(d: std::time::Duration) -> u128 {
        d.as_millis()
    }

    /// One line per frame sent. `other` is everything not accounted for above
    /// -- servicing input between bands, and the protobuf encode -- so a frame
    /// whose cost is not in a named stage still shows up somewhere.
    fn log(&self, total: std::time::Duration) {
        let named = self.probe + self.read + self.convert + self.encode + self.send;
        let other = total.checked_sub(named).unwrap_or_default();
        log::debug!(
            "frame: {} band(s), probe {}, read {}, conv {}, enc {}{}, send {}, other {} = {} ms, {} B",
            self.bands,
            Self::ms(self.probe),
            Self::ms(self.read),
            Self::ms(self.convert),
            Self::ms(self.encode),
            if self.key { " (KEY)" } else { "" },
            Self::ms(self.send),
            Self::ms(other),
            Self::ms(total),
            self.bytes,
        );
    }
}

/// Send the pointer's shape as it looks right now.
///
/// The client cannot draw a pointer, or act on any position, until it has an
/// image: its `setCursorPosition` returns immediately while no image is bound.
///
/// `id` identifies the shape so the client can cache it; the window server's
/// cursor seed serves, since it is exactly "which shape is this". A shape that
/// cannot be read falls back to the built-in arrow rather than to nothing --
/// the pointer being in the right place matters more than its picture.
#[cfg(target_os = "macos")]
fn send_cursor_data(peer: &mut Peer, id: u64) -> io::Result<()> {
    let c = crate::cursor::current().unwrap_or_else(crate::cursor::arrow);
    let mut cd = CursorData::new();
    cd.id = id;
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
fn send_cursor_position(
    peer: &mut Peer,
    tracker: &mut crate::cursor::Tracker,
    last_peer_input: std::time::Instant,
) -> io::Result<()> {
    let (x, y) = crate::input::cursor_position();
    if x < 0.0 {
        return Ok(()); // the position was unreadable; nothing useful to send
    }
    let since = last_peer_input.elapsed().as_millis() as u64;
    if let Some((x, y)) = tracker.update(x as i32, y as i32, since) {
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
/// expensive step. The socket is polled with a short timeout, so an idle screen
/// costs only the dirty-band probe -- and once it has been idle for a few
/// seconds, not even that on every pass. See `IDLE_AFTER`.
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
    // When this peer last sent input. Its own cursor position is held back for
    // a moment afterwards -- see `cursor::SUPPRESS_AFTER_INPUT_MS`.
    #[cfg(target_os = "macos")]
    let mut last_peer_input = std::time::Instant::now()
        - std::time::Duration::from_millis(crate::cursor::SUPPRESS_AFTER_INPUT_MS + 1);
    // Start from a clean slate: see `input::release_modifiers`.
    #[cfg(target_os = "macos")]
    crate::input::release_modifiers();

    // The pointer changes shape over a text field, a resize edge, a link. The
    // seed is one call and changes only when the shape does, so it is polled
    // every pass and the image is fetched only when it has actually moved on.
    #[cfg(target_os = "macos")]
    let mut cursor_seed = crate::cursor::seed();
    #[cfg(target_os = "macos")]
    send_cursor_data(peer, cursor_seed as u32 as u64)?;

    // Liveness. See TEST_DELAY_INTERVAL -- without this the session dies of
    // silence roughly a minute after the screen stops changing.
    let mut delay_sent = std::time::Instant::now();
    let mut delay_outstanding = false;
    // The peer's refresh button. Handled in the loop rather than here because
    // that is where the video state lives.
    #[cfg_attr(not(all(target_os = "macos", not(no_vpx))), allow(unused_mut))]
    let mut refresh_requested = false;

    macro_rules! pump_input {
        () => { pump_input!(true) };
        ($wait:expr) => {{
            #[cfg(target_os = "macos")]
            let alive = drain_input(
                peer,
                $wait,
                &mut delay_outstanding,
                &mut refresh_requested,
                &mut injector,
                &mut cursor_tracker,
                &mut last_peer_input,
            )?;
            #[cfg(not(target_os = "macos"))]
            let alive = drain_input(peer, $wait, &mut delay_outstanding, &mut refresh_requested)?;
            if !alive {
                return Ok(());
            }
        }};
    }

    macro_rules! keep_alive {
        () => {{
            // Re-send after TEST_DELAY_STALE even with one outstanding: a lost
            // or ignored answer must not wedge the timer and take the session
            // down with it.
            let waited = delay_sent.elapsed();
            if waited >= TEST_DELAY_INTERVAL && (!delay_outstanding || waited >= TEST_DELAY_STALE) {
                let mut td = crate::message_proto::TestDelay::new();
                td.time = now_millis();
                td.from_client = false;
                let mut m = Message::new();
                m.set_test_delay(td);
                peer.send(&m)?;
                delay_sent = std::time::Instant::now();
                delay_outstanding = true;
            }
        }};
    }

    // Blocking for POLL_MS at the top of the loop is what stops an idle session
    // spinning, and it is pure latency when the screen is actually changing:
    // measured on a real terminal session, frames cost 70 ms and arrived 103 ms
    // apart, the difference being this wait. Only pay it when the last pass
    // found nothing to send.
    #[cfg_attr(not(all(target_os = "macos", not(no_vpx))), allow(unused_mut))]
    let mut idle_last_pass = true;

    loop {
        pump_input!(idle_last_pass);
        keep_alive!();
        #[cfg(all(target_os = "macos", not(no_vpx)))]
        {
            idle_last_pass = true;
        }

        #[cfg(all(target_os = "macos", not(no_vpx)))]
        if let Some(v) = video.as_mut() {
            // Announce a new size before sending a frame in it: the peer sizes
            // its canvas from what it was last told, so a frame that arrives
            // first is rendered into the wrong geometry.
            if let Some((w, h)) = v.poll_geometry() {
                let mut sd = SwitchDisplay::new();
                sd.display = 0;
                sd.x = 0;
                sd.y = 0;
                sd.width = w;
                sd.height = h;
                let mut mi = Misc::new();
                mi.set_switch_display(sd);
                let mut m = Message::new();
                m.set_misc(mi);
                peer.send(&m)?;
            }

            // The peer asked for a fresh frame; give it a real keyframe.
            if refresh_requested {
                refresh_requested = false;
                log::debug!("peer asked to refresh the video; sending a keyframe");
                v.request_key();
            }

            if let Some(dirty) = v.probe() {
                let frame_start = std::time::Instant::now() - v.t.probe;
                // One band at a time, servicing input in between. A whole
                // frame is ~520 ms of reading and converting; a band is ~33 ms,
                // which is the difference between a mouse that tracks and one
                // that stalls whenever the Dock or a scroll bar animates.
                for (b, moved) in dirty.iter().enumerate() {
                    if !*moved {
                        continue;
                    }
                    v.band(b);
                    // Poll, never wait: see `drain_input`'s `wait` argument.
                    pump_input!(false);
                }
                if let Some((data, key, pts)) = v.encode() {
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
                    // Timed separately: a 100 KB frame is a blocking write, and
                    // if the peer is slow to drain its socket that lands here
                    // rather than in any of the stages above.
                    let ts = std::time::Instant::now();
                    peer.send(&m)?;
                    v.t.send = ts.elapsed();
                    v.t.log(frame_start.elapsed());
                    idle_last_pass = false;
                }
            }
        }

        #[cfg(target_os = "macos")]
        {
            let s = crate::cursor::seed();
            if s != cursor_seed {
                cursor_seed = s;
                log::debug!("pointer changed shape (seed {})", s);
                send_cursor_data(peer, s as u32 as u64)?;
            }
        }

        // Also poll once an iteration: the person at the G5 can move the
        // pointer themselves, and no input event announces that.
        #[cfg(target_os = "macos")]
        send_cursor_position(peer, &mut cursor_tracker, last_peer_input)?;
    }
}

/// Handle every message the peer has already sent, then return.
///
/// Returns false when the peer has gone. Called between the stages of a frame
/// as well as around it: a frame costs hundreds of milliseconds and input read
/// only at frame boundaries makes the mouse stall for exactly that long.
fn drain_input(
    peer: &mut Peer,
    wait: bool,
    delay_outstanding: &mut bool,
    refresh_requested: &mut bool,
    #[cfg(target_os = "macos")] injector: &mut crate::input::Injector,
    #[cfg(target_os = "macos")] cursor_tracker: &mut crate::cursor::Tracker,
    #[cfg(target_os = "macos")] last_peer_input: &mut std::time::Instant,
) -> io::Result<bool> {
    loop {
        // `wait` is what paces an idle loop, and is right exactly once per
        // iteration. Between bands we only want whatever has already arrived:
        // blocking there costs POLL_MS per band, and it cost it twice --
        // skipping empty drains was not enough, because a drain that *did*
        // find input still ended by waiting POLL_MS for more that never came.
        if !wait && !input_waiting(&peer.stream) {
            return Ok(true);
        }
        let msg = match peer.recv() {
            Err(ref e) if is_timeout(e) => return Ok(true),
            Ok(m) => m,
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                log::info!("peer {} disconnected", peer.name);
                return Ok(false);
            }
            Err(e) => return Err(e),
        };
        match msg.union {
            Some(message::Union::mouse_event(me)) => {
                #[cfg(target_os = "macos")]
                {
                    injector.mouse(&me);
                    *last_peer_input = std::time::Instant::now();
                    send_cursor_position(peer, cursor_tracker, *last_peer_input)?;
                }
                let _ = &me;
            }
            Some(message::Union::key_event(ke)) => {
                #[cfg(target_os = "macos")]
                {
                    injector.key(&ke);
                    *last_peer_input = std::time::Instant::now();
                }
                let _ = &ke;
            }
            // Touch gestures. Field 26, backported -- see `Injector::touch`,
            // which drops them on anything older than 10.6.
            #[cfg(target_os = "macos")]
            Some(message::Union::pointer_device_event(pd)) => {
                injector.touch(&pd);
                *last_peer_input = std::time::Instant::now();
            }
            Some(message::Union::test_delay(t)) => {
                if t.from_client {
                    let mut m = Message::new();
                    m.set_test_delay(t);
                    peer.send(&m)?;
                } else {
                    // The answer to one of ours. Both ends of the subtraction
                    // are our own clock, so the figure is a real round trip
                    // however far the peer's clock has drifted.
                    *delay_outstanding = false;
                    let rtt = now_millis() - t.time;
                    if rtt >= 0 {
                        log::debug!("peer round trip: {} ms", rtt);
                    }
                }
            }
            Some(message::Union::misc(mi)) => {
                // The peer's "refresh" button, which arrives in one of two
                // fields depending on what version we claim to be:
                // `refresh_video` (10) below 1.2.4, `refresh_video_display`
                // (31, backported) at or above it. The client picks between
                // them in `is_support_multi_ui_session`, so the field we do not
                // handle is the one whose button does nothing at all.
                if let Some(misc::Union::refresh_video(true)) = mi.union {
                    *refresh_requested = true;
                } else if let Some(misc::Union::refresh_video_display(d)) = mi.union {
                    // The int32 is which display to refresh. This agent serves
                    // one, so any value means the same thing; anything but 0
                    // would mean the peer thinks otherwise, which is worth
                    // hearing about rather than silently obeying.
                    if d != 0 {
                        log::warn!("peer asked to refresh display {}, of which there is only 0", d);
                    }
                    *refresh_requested = true;
                // The peer's options. Worth logging rather than dropping: the
                // client only draws a remote pointer when its "show remote
                // cursor" toggle is on, and this is the only way to tell from
                // here whether it is.
                } else if let Some(misc::Union::option(o)) = mi.union {
                    log::info!(
                        "peer options: show_remote_cursor={:?} image_quality={:?} \
                         disable_clipboard={:?} disable_audio={:?}",
                        o.show_remote_cursor.enum_value_or_default(),
                        o.image_quality.enum_value_or_default(),
                        o.disable_clipboard.enum_value_or_default(),
                        o.disable_audio.enum_value_or_default()
                    );
                    #[cfg(target_os = "macos")]
                    if o.show_remote_cursor.enum_value_or_default() == BoolOption::Yes {
                        // Usually flipped mid-session, long after the shape was
                        // sent at login; without this the peer never gets one.
                        send_cursor_data(peer, crate::cursor::seed() as u32 as u64)?;
                        cursor_tracker.reset();
                        *last_peer_input = std::time::Instant::now()
                            - std::time::Duration::from_millis(
                                crate::cursor::SUPPRESS_AFTER_INPUT_MS + 1,
                            );
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

/// Upstream's version-to-number arithmetic, from `hbb_common::get_version_number`.
///
/// Reimplemented rather than imported because the whole point is to check what
/// *the client* will compute from our string, using the client's own rule.
#[cfg(test)]
fn version_number(v: &str) -> i64 {
    let mut parts = v.split('-');
    let mut n: i64 = 0;
    if let Some(head) = parts.next() {
        let mut last: i64 = 0;
        for x in head.split('.') {
            last = x.parse().unwrap_or(0);
            n = n * 1000 + last;
        }
        // The last component is scaled to leave room for a patch level, so
        // 1.1.10 becomes 1001100 rather than 1001010.
        n -= last;
        n += last * 10;
    }
    if let Some(pre) = parts.next() {
        n += pre.parse::<i64>().unwrap_or(0);
    }
    n
}

#[cfg(test)]
mod version_tests {
    use super::*;

    /// The reported version must claim exactly the gates that are implemented:
    /// no less, or the client keeps to paths we have outgrown, and no more, or
    /// it starts sending things this agent cannot answer. Raising the constant
    /// without doing the work fails here rather than in someone's session.
    #[test]
    fn the_reported_version_claims_what_is_implemented_and_no_more() {
        let ours = version_number(REPORTED_VERSION);

        // Earned: `decide_key` reads KeyEvent.mode, so Map is handled.
        assert!(
            ours >= version_number("1.2.0"),
            "{} is below the keyboard-mode gate, which is implemented",
            REPORTED_VERSION
        );

        // Not earned yet: field 31 is handled, but the rest of what this
        // version gates has not been surveyed.
        assert!(
            ours < version_number("1.2.4"),
            "{} claims the multi-UI-session gate; survey the rest of it first",
            REPORTED_VERSION
        );

        // Not earned: `decide_mouse` takes absolute coordinates and would be
        // handed deltas.
        assert!(
            ours < version_number("1.4.5"),
            "{} claims the relative-mouse gate; handle deltas first",
            REPORTED_VERSION
        );
    }

    /// The arithmetic itself, against the worked example in upstream's own
    /// comment: "1.1.10 -> 1001100". The scaling of the last component is the
    /// part that is easy to get wrong, and getting it wrong quietly would make
    /// the gate check above meaningless.
    #[test]
    fn the_arithmetic_matches_upstreams_worked_example() {
        assert_eq!(version_number("1.1.10"), 1001100);
        assert!(version_number("1.1.10") > version_number("1.1.9"));
        assert!(version_number("1.2.4") > version_number("1.1.8"));
        assert!(version_number("0.1.0") < version_number("1.0.0"));
    }
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
