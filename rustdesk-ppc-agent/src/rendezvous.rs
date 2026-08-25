//! Registering with a rendezvous server, so the G5 is reachable by id.
//!
//! Mirrors the controlled half of upstream's `src/rendezvous_mediator.rs` on
//! blocking I/O: `start_udp`, `handle_intranet_`, `handle_punch_hole` and
//! `handle_request_relay`. Registration itself is plain UDP protobuf with no
//! encryption -- hbbs reads `RegisterPk.uuid` and `.pk` straight off the parsed
//! message -- so none of the TCP key exchange applies here.
//!
//! Its own thread, like `lan`: this socket is silent for minutes at a time and
//! has no business being polled from the session loop.
//!
//! # The three ways a peer arrives
//!
//! All of them start with the server sending us a datagram and end at
//! `session::serve` with an ordinary `TcpStream`. What differs is how that
//! stream is obtained.
//!
//! **`FetchLocalAddr` -- the peer is on our subnet.** Connect TCP to the
//! rendezvous server, note the local `ip:port` the OS picked, send `LocalAddr`
//! naming it, then **drop that socket and bind a listener to the same address**.
//! The server hands our local address to the caller, which dials it directly.
//! Reusing the port is the whole trick, and it is why the socket has to be
//! closed rather than kept: it cannot be both a connection and a listener.
//!
//! **`RequestRelay` -- the caller has already picked a relay.** Tell the server
//! we are on our way (`RelayResponse`), then connect to the relay and announce
//! the same uuid. That stream *is* the peer; nothing is listened for.
//!
//! **`PunchHole` -- the caller wants a direct connection.** We do not punch.
//! Upstream's own controlled side skips punching whenever it judges the attempt
//! hopeless -- symmetric NAT, or a forced relay -- and calls `create_relay` with
//! `initiate = true`, minting a uuid and telling the server to send the caller
//! to the relay instead. We take that branch unconditionally, for a reason that
//! is structural rather than a policy choice: see below.
//!
//! # Why punching is not implemented
//!
//! Hole punching needs the server to know our *public* address. When the
//! rendezvous server is self-hosted behind the same NAT as this machine -- the
//! deployment this was written for -- our registration traffic hairpins at the
//! router and never crosses the WAN, so no external mapping is ever created and
//! the server can only ever record a private address. It cannot hand a remote
//! caller anything reachable, and the punch cannot land no matter how correctly
//! we implement it.
//!
//! Answering with a relay immediately is therefore not a degradation, it is the
//! shortest correct path: it saves the caller the round trip it would otherwise
//! spend waiting for a `PunchHoleSent` that could never help. On a deployment
//! where the server is outside the NAT this costs a direct connection that might
//! have worked, which is the trade recorded in `docs/BACKLOG.md` item 12.
//!
//! # Two protocol details that bite
//!
//! **`RegisterPkResponse::OK` changed number.** This agent's 1.1.8 proto says
//! `OK = 1`; current RustDesk says `OK = 0`. proto3 omits zero-valued fields, so
//! a modern server signalling success sends *no* `result` field at all, and the
//! generated `Default` for the enum happens to be `OK` -- success arrives as 1
//! by luck rather than agreement. `registration_ok` reads the raw i32 and
//! accepts both, so neither vintage can be misread as a failure.
//!
//! **`RequestRelay.licence_key` is backported** (field 6, added after 1.1.8).
//! hbbr only checks it when started with `-k`, so an unkeyed relay -- the
//! common self-hosted case -- accepts us either way, which is why this was
//! invisible for so long. Against a keyed one the request is dropped by
//! *returning* rather than answering, so the symptom is a caller waiting on a
//! relay we appear never to have joined. `--key` sets it; empty is right unless
//! hbbr was started with `-k`. Worth knowing that **hbbs is keyed even with no
//! `-k`**, because it auto-generates `id_ed25519` and uses the public half;
//! hbbr has no such fallback, so the two are configured independently.

use std::io::{self, ErrorKind};
use std::net::{
    Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener, TcpStream, ToSocketAddrs, UdpSocket,
};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use protobuf::Message as _;

use crate::frame::write_frame;
use crate::rendezvous_proto::*;
use crate::session::{self, Identity};

/// Where hbbs listens. `RELAY_PORT - 1`, and the port a bare hostname implies.
pub const RENDEZVOUS_PORT: u16 = 21116;
/// Where hbbr listens, and what a relay address without a port means.
pub const RELAY_PORT: u16 = 21117;

/// How often to re-register while the server is answering. hbbs drops a peer
/// from its online list after 30 s (`REG_TIMEOUT`), so this leaves a peer's
/// worth of margin.
const REG_INTERVAL: Duration = Duration::from_secs(15);
/// How long to wait for a `RegisterPeerResponse` before sending another.
const REG_RETRY: Duration = Duration::from_secs(3);
/// Socket read timeout, which is also how often the loop reconsiders the timers.
const RECV_TIMEOUT: Duration = Duration::from_millis(500);
/// Applies to both hops of a relay: to the rendezvous server, and to the relay.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
/// How long to hold the reused port open waiting for the caller to dial in.
const ACCEPT_TIMEOUT: Duration = Duration::from_secs(10);
/// Poll interval while waiting for that connection. `accept` has no timeout of
/// its own in std, and a blocked accept would stop us registering.
const ACCEPT_POLL: Duration = Duration::from_millis(20);
/// Pause before rebuilding the socket after the loop fails outright.
const RETRY_DELAY: Duration = Duration::from_secs(5);
/// How close together two requests from one peer count as the same one.
/// Upstream's value.
const DEDUP_WINDOW: Duration = Duration::from_millis(100);
/// Rendezvous datagrams are tiny; this is generous.
const MAX_DATAGRAM: usize = 2048;

/// What to tell the server about ourselves, resolved once at startup.
pub struct Registration {
    /// Host or `host:port`. Without a port, `RENDEZVOUS_PORT`.
    pub server: String,
    /// The id a peer types. Must be at least 6 characters or hbbs rejects it
    /// with `UUID_MISMATCH`.
    pub id: String,
    /// Stable across restarts, or the server sees an impostor for our id and
    /// answers `UUID_MISMATCH` for ever.
    pub uuid: Vec<u8>,
    /// The Ed25519 *public* key. Upstream stores its pair as `(secret, public)`
    /// and registers `.1`; peers verify our `SignedId` against it.
    pub public_key: Vec<u8>,
    /// The *server's* key, sent as `RequestRelay.licence_key` when joining a
    /// relay. Empty unless configured, which is right for an unkeyed hbbr.
    pub server_key: String,
    /// A relay to use instead of whichever one the server names. Empty means
    /// "use the server's", which is the normal case.
    pub relay_server: String,
}

/// Which relay to actually join: ours if configured, otherwise the one the
/// server named. Mirrors upstream's `get_relay_server`, which prefers the local
/// option only when it is set -- see `config::relay_server` for when that
/// matters.
fn choose_relay(configured: &str, advertised: &str) -> String {
    if configured.is_empty() {
        advertised.to_owned()
    } else {
        configured.to_owned()
    }
}

/// Register, and answer connection requests, until the process ends.
///
/// Never returns. Every error is logged and retried: a machine that cannot
/// reach its rendezvous server is still perfectly usable by direct IP, so
/// nothing here is worth taking the agent down for.
pub fn serve(reg: Registration, ident: Identity) {
    loop {
        match run(&reg, &ident) {
            Ok(()) => log::warn!("rendezvous: loop ended without error, restarting"),
            Err(e) => log::warn!("rendezvous: {}; retrying in {}s", e, RETRY_DELAY.as_secs()),
        }
        std::thread::sleep(RETRY_DELAY);
    }
}

fn run(reg: &Registration, ident: &Identity) -> io::Result<()> {
    let server = resolve(&reg.server, RENDEZVOUS_PORT)?;
    let socket = UdpSocket::bind(("0.0.0.0", 0))?;
    // Connecting a UDP socket filters out anything not from the server, which
    // is the cheapest possible defence against a stray datagram being parsed.
    socket.connect(server)?;
    // Not fatal where the platform has not got it. Neither IRIX nor Solaris 10
    // has SO_RCVTIMEO -- setsockopt returns ENOPROTOOPT -- and `?` here stopped
    // the registration loop before it sent a single datagram. The loop is
    // *built* on the read timing out, since that is what tells it to resend, so
    // on both the wait moves to poll(2) below rather than being dropped.
    #[cfg(not(any(target_os = "irix", target_os = "solaris")))]
    socket.set_read_timeout(Some(RECV_TIMEOUT))?;
    #[cfg(any(target_os = "irix", target_os = "solaris"))]
    if let Err(e) = socket.set_read_timeout(Some(RECV_TIMEOUT)) {
        log::debug!("no read timeout on this platform ({}); pacing with poll instead", e);
    }
    log::info!("rendezvous: registering with {} as id {}", server, reg.id);

    // `sent` is set while a RegisterPeer is outstanding, `answered` records the
    // last reply. Upstream's rule, minus the public-server backoff: resend after
    // REG_RETRY if unanswered, otherwise refresh every REG_INTERVAL.
    let mut sent: Option<Instant> = None;
    let mut answered: Option<Instant> = None;
    let mut registered = false;
    let mut last_request: Option<(SocketAddr, Instant)> = None;
    let mut buf = [0u8; MAX_DATAGRAM];

    loop {
        let due = match (sent, answered) {
            (Some(t), _) => t.elapsed() >= REG_RETRY,
            (None, Some(t)) => t.elapsed() >= REG_INTERVAL,
            (None, None) => true,
        };
        if due {
            let mut peer = RegisterPeer::new();
            peer.id = reg.id.clone();
            send(&socket, |m| m.set_register_peer(peer))?;
            sent = Some(Instant::now());
        }

        // Where the read timeout above did not take, this is it: wait for a
        // datagram or give up after the same interval, so `due` is reached and
        // the registration is resent.
        #[cfg(any(target_os = "irix", target_os = "solaris"))]
        {
            use std::os::unix::io::AsRawFd;
            if !crate::sys::wait_readable_fd(socket.as_raw_fd(), RECV_TIMEOUT.as_millis() as u64) {
                continue;
            }
        }
        let n = match socket.recv(&mut buf) {
            Ok(n) => n,
            // Both spellings appear across platforms for a read timeout.
            Err(ref e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {
                continue
            }
            Err(e) => return Err(e),
        };
        let msg = match RendezvousMessage::parse_from_bytes(&buf[..n]) {
            Ok(m) => m,
            // Not ours to interpret. Registration is unauthenticated, so this
            // is reachable by anyone who can guess the port.
            Err(_) => continue,
        };

        match msg.union {
            Some(rendezvous_message::Union::register_peer_response(r)) => {
                sent = None;
                answered = Some(Instant::now());
                if r.request_pk {
                    log::info!("rendezvous: server asked for our public key");
                    let mut pk = RegisterPk::new();
                    pk.id = reg.id.clone();
                    pk.uuid = reg.uuid.clone();
                    pk.pk = reg.public_key.clone();
                    send(&socket, |m| m.set_register_pk(pk))?;
                } else if !registered {
                    // The server already had our key, so it will never ask for
                    // it again and no RegisterPkResponse is coming. Saying so
                    // here matters: without it a restart against a server that
                    // remembers us logs "registering" and then nothing at all,
                    // which reads exactly like a hang.
                    registered = true;
                    log::info!("rendezvous: registered, reachable as id {}", reg.id);
                }
            }
            Some(rendezvous_message::Union::register_pk_response(r)) => {
                sent = None;
                answered = Some(Instant::now());
                let code = r.result.value();
                if registration_ok(code) {
                    if !registered {
                        registered = true;
                        log::info!("rendezvous: registered, reachable as id {}", reg.id);
                    }
                } else {
                    registered = false;
                    log::warn!(
                        "rendezvous: the server refused our registration: {}",
                        describe_register_failure(code)
                    );
                }
            }
            Some(rendezvous_message::Union::fetch_local_addr(fla)) => {
                let peer = mangle_decode(&fla.socket_addr);
                if is_duplicate(&mut last_request, peer) {
                    continue;
                }
                log::info!("rendezvous: local-network request from {}", peer);
                let (id, relay_server) =
                    (reg.id.clone(), choose_relay(&reg.relay_server, &fla.relay_server));
                let ident = ident.clone();
                spawn(move || match fetch_local_addr(server, &relay_server, &id, peer) {
                    // Upstream serves this path secure: a peer arriving through
                    // the server always takes part in the key exchange.
                    Ok(stream) => run_session(stream, ident, true, peer),
                    Err(e) => log::warn!("rendezvous: local-network connect back failed: {}", e),
                });
            }
            Some(rendezvous_message::Union::request_relay(rr)) => {
                let peer = mangle_decode(&rr.socket_addr);
                if is_duplicate(&mut last_request, peer) {
                    continue;
                }
                log::info!(
                    "rendezvous: relay request from {} via {}, uuid {}, secure {}",
                    peer,
                    if rr.relay_server.is_empty() { "(unset)" } else { &rr.relay_server },
                    rr.uuid,
                    rr.secure
                );
                let (relay_server, uuid, secure) =
                    (choose_relay(&reg.relay_server, &rr.relay_server), rr.uuid, rr.secure);
                let socket_addr = rr.socket_addr;
                let ident = ident.clone();
                let key = reg.server_key.clone();
                spawn(move || match relay(server, &relay_server, &uuid, &socket_addr, None, &key) {
                    Ok(stream) => run_session(stream, ident, secure, peer),
                    Err(e) => log::warn!("rendezvous: relay connect failed: {}", e),
                });
            }
            Some(rendezvous_message::Union::punch_hole(ph)) => {
                // See the module header: we answer with a relay we initiate
                // ourselves rather than attempting a punch that cannot land.
                let peer = mangle_decode(&ph.socket_addr);
                if is_duplicate(&mut last_request, peer) {
                    continue;
                }
                let uuid = uuid_v4();
                log::info!(
                    "rendezvous: punch request from {}; answering with relay {} (uuid {})",
                    peer,
                    if ph.relay_server.is_empty() { "(unset)" } else { &ph.relay_server },
                    uuid
                );
                let (relay_server, id) =
                    (choose_relay(&reg.relay_server, &ph.relay_server), reg.id.clone());
                let socket_addr = ph.socket_addr;
                let ident = ident.clone();
                let key = reg.server_key.clone();
                spawn(move || {
                    match relay(server, &relay_server, &uuid, &socket_addr, Some(&id), &key) {
                        Ok(stream) => run_session(stream, ident, true, peer),
                        Err(e) => log::warn!("rendezvous: relay connect failed: {}", e),
                    }
                });
            }
            Some(rendezvous_message::Union::configure_update(cu)) => {
                // Only meaningful to a client that can be told about other
                // rendezvous servers. We have exactly one, by configuration.
                log::debug!("rendezvous: ignoring a config update (serial {})", cu.serial);
            }
            other => log::debug!("rendezvous: ignoring {:?}", other),
        }
    }
}

/// Connect back for a peer on our own subnet, and return the peer's stream.
///
/// # Why this does not reuse a port, and upstream does
///
/// Upstream takes the local port of its connection to the rendezvous server,
/// closes it, and binds a listener to that exact address. It has to: the same
/// function serves hole punching, where the whole point is that the outgoing
/// connection has already made the NAT map that port, so the caller's packets
/// have somewhere to land.
///
/// **This agent never punches** (see the module header), so nothing depends on
/// the advertised port being the one that talked to the server -- the caller is
/// on our subnet and dials us directly, with no NAT in between. Listening on a
/// port of our own is therefore equivalent, and it avoids a real failure: the
/// reuse needs `SO_REUSEADDR` on *both* sockets, and std cannot set options on
/// an outgoing `TcpStream` before it connects. Binding the port back was
/// measured failing with `EADDRINUSE`, because our end of the just-closed
/// connection is in `FIN_WAIT`, which `SO_REUSEADDR` does not cover -- only
/// `TIME_WAIT`.
///
/// The address still has to name an interface the caller can reach, so the IP
/// is taken from the connection to the server. Only the port is ours.
fn fetch_local_addr(
    server: SocketAddr,
    relay_server: &str,
    id: &str,
    peer: SocketAddr,
) -> io::Result<TcpStream> {
    // Bound before we announce it, so the caller cannot arrive first.
    let listener = TcpListener::bind(("0.0.0.0", 0))?;
    let port = listener.local_addr()?.port();

    let mut stream = TcpStream::connect_timeout(&server, CONNECT_TIMEOUT)?;
    let local = SocketAddr::new(stream.local_addr()?.ip(), port);

    let mut la = LocalAddr::new();
    la.id = id.to_owned();
    la.socket_addr = mangle_encode(peer);
    la.local_addr = mangle_encode(local);
    la.relay_server = relay_server.to_owned();
    la.version = session::REPORTED_VERSION.to_owned();
    let mut msg = RendezvousMessage::new();
    msg.set_local_addr(la);
    write_frame(&mut stream, &msg.write_to_bytes()?)?;
    drop(stream);

    log::info!("rendezvous: listening on {} for the caller", local);
    accept_one(&listener, ACCEPT_TIMEOUT)
}

/// Join a relay for `peer`, and return the peer's stream.
///
/// `initiate` carries our id when *we* are choosing the relay rather than
/// answering a caller who already chose one; upstream sets the extra fields
/// only in that case, and they are what tell the server to send the caller
/// after us.
fn relay(
    server: SocketAddr,
    relay_server: &str,
    uuid: &str,
    socket_addr: &[u8],
    initiate: Option<&str>,
    server_key: &str,
) -> io::Result<TcpStream> {
    // Checked before we announce anything: telling the server we are on our way
    // and then failing to arrive leaves the caller waiting on a relay we were
    // never going to join.
    if relay_server.is_empty() {
        return Err(io::Error::new(
            ErrorKind::InvalidInput,
            "the server named no relay, and this agent has no default to fall back on",
        ));
    }
    let addr = resolve(relay_server, RELAY_PORT)?;

    let mut rr = RelayResponse::new();
    rr.socket_addr = socket_addr.to_vec();
    rr.version = session::REPORTED_VERSION.to_owned();
    if let Some(id) = initiate {
        rr.uuid = uuid.to_owned();
        rr.relay_server = relay_server.to_owned();
        rr.set_id(id.to_owned());
    }
    let mut msg = RendezvousMessage::new();
    msg.set_relay_response(rr);
    let mut to_server = TcpStream::connect_timeout(&server, CONNECT_TIMEOUT)?;
    write_frame(&mut to_server, &msg.write_to_bytes()?)?;
    drop(to_server);

    let mut stream = TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT)?;
    // A keyed hbbr rejects a wrong key by *returning* -- no error comes back,
    // the caller simply waits for a peer that never joins -- so a mismatch here
    // looks exactly like an unreachable agent. Sent unconditionally, as
    // upstream does: an unkeyed relay ignores the field.
    let mut req = RequestRelay::new();
    req.licence_key = server_key.to_owned();
    req.uuid = uuid.to_owned();
    let mut msg = RendezvousMessage::new();
    msg.set_request_relay(req);
    write_frame(&mut stream, &msg.write_to_bytes()?)?;
    log::info!("rendezvous: joined relay {} for uuid {}", addr, uuid);
    Ok(stream)
}

/// Everything a connection request needs runs off the registration loop.
///
/// Not just the session -- the *connect back* too. Both hops of a relay, and
/// waiting for a local caller to dial in, can take fifteen seconds between
/// them, and hbbs drops a peer that has not registered for thirty. Doing this
/// inline made the agent go offline while it was answering the door.
fn spawn<F: FnOnce() + Send + 'static>(f: F) {
    std::thread::spawn(f);
}

/// Serve one peer. Already on its own thread; see `spawn`.
fn run_session(stream: TcpStream, mut ident: Identity, secure: bool, peer: SocketAddr) {
    // The route the peer took decides this, not our own `--secure` flag: a peer
    // arriving through the server always takes part in the key exchange.
    ident.secure = secure;
    if let Err(e) = session::serve(stream, &ident) {
        log::warn!("session with {} ended: {}", peer, e);
    }
}

/// Has this peer just asked for the same thing?
///
/// hbbs forwards a caller's retries, and a caller that is impatient sends
/// several. Upstream drops a repeat within 100 ms; without it one caller can
/// start several sessions at once, which on this machine is expensive.
fn is_duplicate(last: &mut Option<(SocketAddr, Instant)>, peer: SocketAddr) -> bool {
    if let Some((addr, at)) = *last {
        if addr == peer && at.elapsed() < DEDUP_WINDOW {
            log::debug!("rendezvous: ignoring a repeated request from {}", peer);
            return true;
        }
    }
    *last = Some((peer, Instant::now()));
    false
}

/// `OK` is 1 in the 1.1.8 proto and 0 in current RustDesk, and proto3 omits a
/// zero, so both "absent" and either number mean success. See the module header.
fn registration_ok(code: i32) -> bool {
    code == 0 || code == 1
}

fn describe_register_failure(code: i32) -> &'static str {
    match code {
        2 => "UUID_MISMATCH -- another machine holds this id, or our uuid changed",
        3 => "ID_EXISTS",
        4 => "TOO_FREQUENT -- registering again too soon",
        5 => "INVALID_ID_FORMAT",
        6 => "NOT_SUPPORT -- this server does not accept registration on this transport",
        7 => "SERVER_ERROR",
        8 => "NOT_DEPLOYED -- the server requires the device to be enrolled first",
        _ => "an unrecognised result",
    }
}

fn send<F: FnOnce(&mut RendezvousMessage)>(socket: &UdpSocket, fill: F) -> io::Result<()> {
    let mut msg = RendezvousMessage::new();
    fill(&mut msg);
    let bytes = msg.write_to_bytes()?;
    socket.send(&bytes)?;
    Ok(())
}

/// Wait for one inbound connection, without blocking for ever.
fn accept_one(listener: &TcpListener, timeout: Duration) -> io::Result<TcpStream> {
    listener.set_nonblocking(true)?;
    let start = Instant::now();
    loop {
        match listener.accept() {
            Ok((stream, from)) => {
                stream.set_nonblocking(false)?;
                log::info!("rendezvous: caller connected from {}", from);
                return Ok(stream);
            }
            Err(ref e) if e.kind() == ErrorKind::WouldBlock => {
                if start.elapsed() >= timeout {
                    return Err(io::Error::new(
                        ErrorKind::TimedOut,
                        "the caller never connected",
                    ));
                }
                std::thread::sleep(ACCEPT_POLL);
            }
            Err(e) => return Err(e),
        }
    }
}

/// `host` or `host:port`, resolved to one address.
///
/// IPv4 only, deliberately: `AddrMangle` cannot express a v6 address in the
/// 1.1.8 encoding, so a v6 rendezvous address would produce peers we could
/// never name back to the server.
fn resolve(host: &str, default_port: u16) -> io::Result<SocketAddr> {
    let with_port = if host.rfind(':').map_or(false, |i| host[i + 1..].parse::<u16>().is_ok()) {
        host.to_owned()
    } else {
        format!("{}:{}", host, default_port)
    };
    with_port
        .to_socket_addrs()?
        .find(|a| a.is_ipv4())
        .ok_or_else(|| {
            io::Error::new(
                ErrorKind::NotFound,
                format!("no IPv4 address for {}", with_port),
            )
        })
}

/// A version-4 UUID in the usual text form.
///
/// Only ever used as a key both ends agree on, but the relay is shared with
/// stock clients, so it is worth looking like one of theirs.
fn uuid_v4() -> String {
    let mut b = [0u8; 16];
    sodiumoxide::randombytes::randombytes_into(&mut b);
    b[6] = (b[6] & 0x0f) | 0x40; // version 4
    b[8] = (b[8] & 0x3f) | 0x80; // variant 1
    let hex: String = b.iter().map(|x| format!("{:02x}", x)).collect();
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

// --- AddrMangle ------------------------------------------------------------
//
// Upstream's `hbb_common::AddrMangle`, which folds a v4 address and a
// microsecond timestamp together so a socket address is not plainly visible in
// a datagram. Reimplemented rather than imported: the agent does not depend on
// hbb_common, and the encoding is nine lines. The wire format is byte-for-byte
// identical to both the 1.1.8 and the current version -- only a v6 branch was
// added since, which this agent has no way to use.
//
// **Upstream computes this in `u128`; we use two `u64` halves, and that is not
// a stylistic choice.** 32-bit PowerPC gcc has no `__int128`, so mrustc emits a
// software `uint128_t` -- `struct { uint64_t lo, hi; }` -- and the byte-swap
// that `u128::to_le_bytes` owes a big-endian machine does not survive it. The
// first build to reach the G5 decoded a peer at `192.168.99.153` as
// `0.128.63.223`, while the same bytes on x86 decoded correctly.
//
// That was survivable in the relay paths, which pass `socket_addr` through as
// opaque bytes, and fatal in the local-network one, which re-encodes both the
// peer's address and our own -- so the caller would have been handed nonsense
// to dial. The intermediate value needs 82 bits, so the arithmetic is done as
// an explicit (hi, lo) pair: every operation is then native on any target, and
// `u64::to_le_bytes` is the only endian-sensitive step left.
//
// `mangle_matches_the_wire_format` pins this with a vector computed
// independently, which is the test that would have caught it.

/// Split the 82-bit intermediate into the two halves the wire format wants.
///
/// `v = ((ip + tm) << 49) | (tm << 17) | (port + (tm & 0xFFFF))`, where the top
/// term needs 33 bits and so straddles bit 64.
fn mangle_parts(ip: u32, port: u16, tm: u32) -> (u64, u64) {
    let a = ip as u64 + tm as u64; // <= 2^33
    let p = port as u64 + (tm as u64 & 0xFFFF); // <= 0x1FFFE, 17 bits
    // `a << 49` keeps only a's low 15 bits in `lo`; the rest lands in `hi`.
    let lo = p | ((tm as u64) << 17) | (a << 49);
    let hi = a >> 15;
    (hi, lo)
}

/// Encode a v4 address. A v6 address yields an empty vector, which the server
/// treats as absent, rather than a v6 encoding the 1.1.8 proto cannot express.
fn mangle_encode(addr: SocketAddr) -> Vec<u8> {
    let v4 = match addr {
        SocketAddr::V4(v4) => v4,
        SocketAddr::V6(_) => return Vec::new(),
    };
    let tm = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or(Duration::ZERO)
        .as_micros() as u32;
    let ip = u32::from_le_bytes(v4.ip().octets());
    let (hi, lo) = mangle_parts(ip, v4.port(), tm);

    let mut bytes = [0u8; 16];
    bytes[..8].copy_from_slice(&lo.to_le_bytes());
    bytes[8..].copy_from_slice(&hi.to_le_bytes());
    let mut trailing_zeroes = 0;
    for b in bytes.iter().rev() {
        if *b == 0 {
            trailing_zeroes += 1;
        } else {
            break;
        }
    }
    bytes[..(16 - trailing_zeroes)].to_vec()
}

/// Decode what `mangle_encode` produced.
///
/// The `0xFFFFFF` mask looks wrong -- it reaches seven bits above the port --
/// but the surplus lives above bit 16 and the `as u16` truncates it away. It is
/// upstream's mask, and changing it would desynchronise us from every peer.
fn mangle_decode(bytes: &[u8]) -> SocketAddr {
    let unspecified = SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 0));
    if bytes.is_empty() || bytes.len() > 16 {
        // >16 is the v6 encoding, which we never advertise and cannot answer.
        return unspecified;
    }
    let mut padded = [0u8; 16];
    padded[..bytes.len()].copy_from_slice(bytes);
    let mut half = [0u8; 8];
    half.copy_from_slice(&padded[..8]);
    let lo = u64::from_le_bytes(half);
    half.copy_from_slice(&padded[8..]);
    let hi = u64::from_le_bytes(half);

    // The mirror of `mangle_parts`: `tm` is bits 17..49, and the top term
    // straddles bit 64, so it is reassembled from both halves.
    let tm = (lo >> 17) & (u32::MAX as u64);
    let a = (lo >> 49) | (hi << 15);
    let ip = (a.wrapping_sub(tm) as u32).to_le_bytes();
    let port = (lo & 0xFFFFFF).wrapping_sub(tm & 0xFFFF);
    SocketAddr::V4(SocketAddrV4::new(
        Ipv4Addr::new(ip[0], ip[1], ip[2], ip[3]),
        port as u16,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    /// The default has to be "use whatever the server said", because that is
    /// the only relay a stock deployment has: hbbs advertises one to peers that
    /// do not set their own. An override that applied when unset would break
    /// every working setup.
    #[test]
    fn relay_override_applies_only_when_configured() {
        assert_eq!(choose_relay("", "relay.example.org"), "relay.example.org");
        assert_eq!(choose_relay("mine.example.org", "relay.example.org"), "mine.example.org");
        // The case this exists for: the server names something a remote caller
        // cannot reach, and ours wins anyway.
        assert_eq!(choose_relay("public.example.org", "192.168.99.1"), "public.example.org");
        // Both empty is not an error here -- `relay` rejects an empty target
        // with a message, rather than dialling nothing.
        assert_eq!(choose_relay("", ""), "");
    }

    #[test]
    fn mangle_round_trips() {
        for addr in [
            "192.168.99.116:21118",
            "1.2.3.4:1",
            "255.255.255.255:65535",
            "0.0.0.1:80",
            "72.139.206.146:17811",
        ] {
            let a: SocketAddr = addr.parse().unwrap();
            assert_eq!(mangle_decode(&mangle_encode(a)), a, "{}", addr);
        }
    }

    /// The wire format, pinned against a vector computed independently of this
    /// code. A round-trip test cannot catch a byte-order fault -- it agrees
    /// with itself -- and that is exactly the fault the first PowerPC build
    /// had, decoding a peer at 192.168.99.153 as 0.128.63.223. See the note
    /// above `mangle_parts`.
    #[test]
    fn mangle_matches_the_wire_format() {
        let bytes = [
            0xf6, 0xa8, 0xf0, 0xac, 0x68, 0x24, 0x70, 0xfe, 0x2f, 0x0d, 0x01,
        ];
        assert_eq!(
            mangle_decode(&bytes),
            "192.168.99.116:21118".parse::<SocketAddr>().unwrap()
        );

        // And the encoder produces those same bytes for that address at the
        // timestamp they were generated with.
        let (hi, lo) = mangle_parts(
            u32::from_le_bytes([192, 168, 99, 116]),
            21118,
            0x1234_5678,
        );
        let mut out = [0u8; 16];
        out[..8].copy_from_slice(&lo.to_le_bytes());
        out[8..].copy_from_slice(&hi.to_le_bytes());
        assert_eq!(&out[..bytes.len()], &bytes[..]);
        assert!(out[bytes.len()..].iter().all(|b| *b == 0), "trailing padding");
    }

    /// Port 0 and address 0 encode to all-zero bytes, which the trailing-zero
    /// trim reduces to nothing. Upstream has the same behaviour; the point of
    /// the test is that decoding it does not panic.
    #[test]
    fn mangle_survives_degenerate_input() {
        assert_eq!(mangle_decode(&[]).port(), 0);
        assert_eq!(mangle_decode(&[0u8; 18]).port(), 0);
    }

    /// A v6 address has no representation here, and must not be encoded as if
    /// it had one.
    #[test]
    fn mangle_refuses_ipv6() {
        let a: SocketAddr = "[::1]:21118".parse().unwrap();
        assert!(mangle_encode(a).is_empty());
    }

    /// Both proto vintages must read as success, including the modern server's
    /// omitted field. See the module header.
    #[test]
    fn both_spellings_of_ok_are_accepted() {
        assert!(registration_ok(0), "current RustDesk sends OK = 0");
        assert!(registration_ok(1), "the 1.1.8 proto this agent speaks says OK = 1");
        for bad in 2..=8 {
            assert!(!registration_ok(bad), "{} is a failure", bad);
        }
    }

    /// An absent `result` field is what a modern server actually sends on
    /// success, so the parse of an empty message must not read as a refusal.
    #[test]
    fn an_absent_result_field_is_success() {
        let parsed = RegisterPkResponse::parse_from_bytes(&[]).unwrap();
        assert!(registration_ok(parsed.result.value()));
    }

    #[test]
    fn resolve_defaults_the_port_and_keeps_an_explicit_one() {
        assert_eq!(resolve("127.0.0.1", RENDEZVOUS_PORT).unwrap().port(), 21116);
        assert_eq!(resolve("127.0.0.1:21117", RENDEZVOUS_PORT).unwrap().port(), 21117);
        // A hostname with a colon but no numeric port is not a port.
        assert!(resolve("127.0.0.1:nonsense", RENDEZVOUS_PORT).is_err());
    }

    #[test]
    fn uuid_looks_like_a_v4_uuid() {
        let u = uuid_v4();
        assert_eq!(u.len(), 36);
        let parts: Vec<&str> = u.split('-').collect();
        assert_eq!(parts.iter().map(|p| p.len()).collect::<Vec<_>>(), vec![8, 4, 4, 4, 12]);
        assert!(u.chars().all(|c| c.is_ascii_hexdigit() || c == '-'));
        assert_eq!(parts[2].as_bytes()[0], b'4', "version nibble");
        assert!(matches!(parts[3].as_bytes()[0], b'8' | b'9' | b'a' | b'b'), "variant");
        assert_ne!(uuid_v4(), u, "two uuids must differ");
    }

    /// A repeat from the same caller is dropped; a different caller never is,
    /// or two people connecting at once would lock each other out.
    #[test]
    fn only_an_immediate_repeat_from_the_same_peer_is_dropped() {
        let a: SocketAddr = "10.0.0.1:5000".parse().unwrap();
        let b: SocketAddr = "10.0.0.2:5000".parse().unwrap();
        let mut last = None;

        assert!(!is_duplicate(&mut last, a), "the first request is never a repeat");
        assert!(is_duplicate(&mut last, a), "an immediate repeat is dropped");
        assert!(!is_duplicate(&mut last, b), "a different peer is not a repeat");
        assert!(!is_duplicate(&mut last, a), "and that displaced the remembered one");

        // Far enough in the past that the window has closed.
        last = Some((a, Instant::now() - DEDUP_WINDOW * 2));
        assert!(!is_duplicate(&mut last, a), "a later request is a new one");
    }

    #[test]
    fn register_failures_are_all_described() {
        for code in 2..=8 {
            assert!(!describe_register_failure(code).contains("unrecognised"));
        }
        assert!(describe_register_failure(99).contains("unrecognised"));
    }
}
