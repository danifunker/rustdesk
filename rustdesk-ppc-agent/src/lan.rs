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

use std::net::UdpSocket;

use protobuf::Message as _;

use crate::rendezvous_proto::*;

/// `RENDEZVOUS_PORT + 3`, which is where clients broadcast.
pub const DISCOVERY_PORT: u16 = 21119;

/// What this machine answers with.
pub struct Announcement {
    pub id: String,
    pub hostname: String,
    pub username: String,
}

/// Answer discovery broadcasts until the process ends.
///
/// Never returns. Errors are logged and swallowed rather than propagated: a
/// machine that cannot be discovered is still perfectly usable by IP, so
/// nothing here is worth taking the agent down for.
pub fn serve(me: Announcement) {
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
        let mut pong = PeerDiscovery::new();
        pong.cmd = "pong".to_owned();
        pong.id = me.id.clone();
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
