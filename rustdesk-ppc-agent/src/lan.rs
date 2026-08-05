//! Answering the broadcast that puts this machine in a client's local list.
//!
//! Mirrors the listener half of upstream's `src/lan.rs`. A client looking for
//! peers broadcasts a `PeerDiscovery { cmd: "ping" }` to UDP
//! `RENDEZVOUS_PORT + 3`, and every machine that answers `"pong"` appears in
//! its list. Without this the G5 is reachable only by typing its IP.
//!
//! `PeerDiscovery` is field 22 of `RendezvousMessage` and does not exist in the
//! 1.1.8 proto, so it is backported the same way `vp8s` and
//! `pointer_device_event` were -- see `protos/rendezvous.proto`.
//!
//! Deliberately its own thread with nothing shared but three strings. The
//! session loop is a tight budget of milliseconds on a slow machine, and a
//! socket that is silent for hours has no business being polled inside it.

use std::net::{IpAddr, SocketAddr, UdpSocket};

use protobuf::Message as _;

use crate::rendezvous_proto::*;

/// `RENDEZVOUS_PORT + 3`, which is where clients broadcast.
pub const DISCOVERY_PORT: u16 = 21119;

/// The port a client dials when the id it was given is a bare IP.
///
/// `client.rs` calls `check_port(peer, RELAY_PORT + 1)`, and RELAY_PORT is
/// 21117, so a bare IP always means 21118. Discovery cannot express any other:
/// the only two forms the client will connect directly to are a bare IP and a
/// *domain* with a port, and the latter's regex demands a letter TLD, so
/// `192.168.1.5:9999` matches neither.
pub const DIRECT_PORT: u16 = 21118;

/// What this machine answers with.
pub struct Announcement {
    /// The agent's own RustDesk id. Only used to recognise our own broadcast --
    /// what we *advertise* is an IP address, for the reason in `serve`.
    pub id: String,
    pub hostname: String,
    pub username: String,
    /// Where the agent is actually listening. Discovery can only advertise a
    /// machine reachable on `DIRECT_PORT`; see `serve`.
    pub port: u16,
}

/// Which of our addresses routes towards `peer`.
///
/// Connecting a UDP socket sends nothing; it just asks the routing table which
/// local address would be used, which is the interface the client can reach us
/// on. Upstream does the same thing for the same reason.
fn local_ip_towards(peer: SocketAddr) -> Option<IpAddr> {
    let s = UdpSocket::bind(("0.0.0.0", 0)).ok()?;
    s.connect(peer).ok()?;
    s.local_addr().ok().map(|a| a.ip())
}

/// Answer discovery broadcasts until the process ends.
///
/// Never returns. Errors are logged and swallowed rather than propagated: a
/// machine that cannot be discovered is still perfectly usable by IP, so
/// nothing here is worth taking the agent down for.
pub fn serve(me: Announcement) {
    // A machine that appears in the list and then refuses to connect is worse
    // than one that never appears: the failure looks like a broken agent rather
    // than a configuration it cannot express.
    if me.port != DIRECT_PORT {
        log::warn!(
            "lan discovery disabled: the agent is on port {}, and a discovered \
             peer can only be dialled on {}",
            me.port,
            DIRECT_PORT
        );
        return;
    }
    let socket = match UdpSocket::bind(("0.0.0.0", DISCOVERY_PORT)) {
        Ok(s) => s,
        Err(e) => {
            // Another RustDesk on the same machine is the usual cause, and it
            // will answer for both of us.
            log::warn!("lan discovery disabled: cannot bind udp/{}: {}", DISCOVERY_PORT, e);
            return;
        }
    };
    log::info!("lan discovery listening on udp/{}", DISCOVERY_PORT);

    let mut buf = [0u8; 2048];
    loop {
        let (len, from) = match socket.recv_from(&mut buf) {
            Ok(v) => v,
            Err(e) => {
                log::debug!("lan discovery recv failed: {}", e);
                continue;
            }
        };
        let msg = match RendezvousMessage::parse_from_bytes(&buf[..len]) {
            Ok(m) => m,
            // Anything else on this port is not ours to interpret.
            Err(_) => continue,
        };
        let ping = match msg.union {
            Some(rendezvous_message::Union::peer_discovery(p)) => p,
            _ => continue,
        };
        if ping.cmd != "ping" || ping.id == me.id {
            // Our own broadcast, or a pong someone else sent. Answering either
            // would put us in a conversation with ourselves.
            continue;
        }
        // **The id must be our IP address, not the agent's RustDesk id.**
        //
        // It is what the client dials, and it only connects directly when the
        // id *is* an IP (`client.rs`: `if is_ip_str(peer)`). Given anything
        // else it asks a rendezvous server to resolve it -- and this agent is
        // registered with none, so the machine appeared in the list and then
        // failed to connect, with nothing arriving here to show for it.
        //
        // Reporting the address is also the honest answer for a direct-IP-only
        // agent: it is the only place we can actually be reached. The hostname
        // still identifies the machine in the client's list.
        //
        // **This is the line to change when a rendezvous server exists.** Once
        // the agent registers with one, `me.id` becomes resolvable and is the
        // better answer: an id survives a DHCP lease expiring and works from
        // another subnet, neither of which an address does. Until then it
        // resolves nowhere, and advertising it is what made the machine appear
        // in the list and then refuse to connect.
        let ip = match local_ip_towards(from) {
            Some(ip) => ip.to_string(),
            None => {
                log::debug!("lan discovery: no route back to {}, not answering", from);
                continue;
            }
        };
        let mut pong = PeerDiscovery::new();
        pong.cmd = "pong".to_owned();
        pong.id = ip;
        pong.hostname = me.hostname.clone();
        pong.username = me.username.clone();
        pong.platform = "Mac OS".to_owned();
        // `mac` is left empty on purpose. Upstream fills it so a client can
        // wake the machine over the network, which this agent cannot support
        // anyway: waking it means it was off, and an agent that is off cannot
        // have answered. Discovery itself does not use the field.
        let mut out = RendezvousMessage::new();
        out.set_peer_discovery(pong);
        match out.write_to_bytes() {
            Ok(bytes) => {
                if let Err(e) = socket.send_to(&bytes, from) {
                    log::debug!("lan discovery reply to {} failed: {}", from, e);
                } else {
                    log::info!("lan discovery: answered {}", from);
                }
            }
            Err(e) => log::warn!("lan discovery: could not encode a reply: {}", e),
        }
    }
}
