//! Reporting in to a console, so the machine appears in its device list.
//!
//! Mirrors the controlled half of upstream's `src/hbbs_http/sync.rs` on
//! blocking I/O: the periodic heartbeat, and the inventory upload the server
//! can ask for. Written against CortenDesk's `SyncController`, which is the
//! open implementation of the same contract stock RustDesk clients speak.
//!
//! # Why this exists at all
//!
//! `rendezvous` makes the machine **reachable**; it does not make it
//! **visible**. A console builds its device list from this HTTP API and not
//! from hbbs registration, so an agent that only registers is connectable by
//! id and absent from the list -- which looks exactly like a broken agent.
//! These two POSTs are the whole difference.
//!
//! # The contract
//!
//! Both endpoints are **tokenless**: no login, no bearer token, no enrolment
//! secret. That is what keeps this module small, and it is a deliberate
//! property of the upstream protocol rather than an oversight of ours.
//!
//! **`POST /api/sysinfo`** is what creates the device row: `id`, `uuid`, `cpu`,
//! `memory`, `os`, `hostname`, `username`, `version`. The reply is **plain
//! text**, not JSON -- `SYSINFO_UPDATED` on success, `ID_NOT_FOUND` if we sent
//! no id.
//!
//! **`POST /api/heartbeat`** keeps it online: `id` and `uuid`. The reply is
//! JSON, and the only key the agent acts on is `sysinfo` -- present when the
//! server does not recognise us, which is its way of asking for the inventory.
//!
//! # Three details that decide whether this works
//!
//! **The uuid is base64 and must never change.** The server pins the first
//! uuid it sees for an id and silently ignores a heartbeat whose uuid differs,
//! because these endpoints are unauthenticated and that pinning is the only
//! spoof guard they have. A regenerated uuid does not re-register, it goes
//! quiet -- the same trap `Config::uuid` documents for hbbs, with a different
//! server enforcing it.
//!
//! **Sixty seconds decides "online".** That is the console's default window
//! since the last heartbeat, so the fifteen-second interval here is not a
//! round number -- it is three missed beats of slack. Raising it past sixty
//! makes the machine flicker offline between beats.
//!
//! **Only `hostname` drives the re-request.** A device row whose hostname is
//! empty gets asked for its inventory on every heartbeat, so a blank hostname
//! would be a permanent request loop. Empty `cpu` or `memory` are harmless,
//! which is why `sys` is allowed to return nothing for them off a Mac.
//!
//! Its own thread, like `lan` and `rendezvous`: it sleeps for fifteen seconds
//! at a time and has no business being polled from the session loop.

use std::time::Duration;

use crate::http::{self, Url};
use crate::json;

/// Three missed beats before the console's default sixty-second window closes.
pub const HEARTBEAT_INTERVAL: Duration = Duration::from_secs(15);

const SYSINFO_PATH: &str = "/api/sysinfo";
const HEARTBEAT_PATH: &str = "/api/heartbeat";

/// The exact strings `/api/sysinfo` answers with. Not JSON, and not a status
/// code: the body is the result.
const SYSINFO_OK: &str = "SYSINFO_UPDATED";
const SYSINFO_NO_ID: &str = "ID_NOT_FOUND";

/// What this machine tells the console about itself.
pub struct Enrolment {
    /// The console's base URL. A bare host is taken as https; see `http::Url`.
    pub api_server: String,
    pub id: String,
    /// The same raw uuid `rendezvous` registers with. Encoded to base64 here,
    /// which is the form the HTTP API expects and pins.
    pub uuid: Vec<u8>,
    pub hostname: String,
    pub username: String,
    /// A CA bundle to verify an https console against. Empty means "look in
    /// the usual places" -- see `http::CaBundle`.
    pub ca_bundle: String,
}

/// Report in until the process ends.
///
/// Never returns. Every error is logged and swallowed: a machine missing from
/// the device list is still perfectly connectable by id, so nothing in here is
/// worth taking the agent down for.
pub fn serve(me: Enrolment) {
    let url = match Url::parse(&me.api_server) {
        Ok(u) => u,
        Err(e) => {
            log::error!("console disabled: {:?} is not a URL ({})", me.api_server, e);
            return;
        }
    };
    // Loaded once, and only when it will be used: an http console needs no
    // certificates, and failing to start over a missing bundle it would never
    // have opened would be wrong.
    let ca = if url.secure {
        match http::CaBundle::load(&me.ca_bundle) {
            Ok(b) => {
                log::info!("console: verifying certificates against {}", b.source);
                Some(b)
            }
            Err(e) => {
                log::error!("console disabled: {}", e);
                return;
            }
        }
    } else {
        // Worth saying once. Everything the heartbeat carries -- the id, the
        // hostname -- crosses the network in the clear on this path.
        log::warn!("console: {} is plain http, so this machine's id and hostname are sent unencrypted", url);
        None
    };
    let uuid = crate::config::base64_encode(&me.uuid);
    // Neither body changes during a run, so neither is rebuilt per tick.
    let inventory = sysinfo_body(
        &me,
        &uuid,
        &crate::sys::cpu_description(),
        &crate::sys::memory_description(),
        &crate::sys::os_description(),
    );
    let beat = heartbeat_body(&me, &uuid);
    log::info!("console: reporting to {} as id {}", url, me.id);

    let mut fault = Fault::new();
    // Introduce ourselves rather than waiting to be asked. The server would
    // request the inventory on the first heartbeat anyway; sending it now is
    // the difference between appearing in the list at once and appearing in
    // fifteen seconds.
    post_sysinfo(&url, &inventory, ca.as_ref(), &mut fault);

    loop {
        std::thread::sleep(HEARTBEAT_INTERVAL);
        match heartbeat(&url, &beat, ca.as_ref()) {
            Ok(wants_inventory) => {
                fault.clear("console: reachable again");
                if wants_inventory {
                    log::info!("console: asked for the inventory");
                    post_sysinfo(&url, &inventory, ca.as_ref(), &mut fault);
                }
            }
            Err(msg) => fault.raise(&msg),
        }
    }
}

/// One heartbeat. `Ok(true)` means the console asked for the inventory.
///
/// Split from the loop so it can be driven against a server rather than only
/// reasoned about: the failure this guards against is a request the console
/// accepts and ignores, which no unit test on the body alone can see.
fn heartbeat(url: &Url, body: &str, ca: Option<&http::CaBundle>) -> Result<bool, String> {
    match http::post_json(url, HEARTBEAT_PATH, body, ca) {
        // The one key we act on. Anything else in the reply -- `disconnect`,
        // `strategy` -- belongs to features this agent does not have, and is
        // ignored rather than half-honoured.
        Ok(r) if r.ok() => Ok(json::field(&r.body, "sysinfo").map_or(false, json::truthy)),
        Ok(r) => Err(describe(HEARTBEAT_PATH, &r)),
        Err(e) => Err(format!("console: heartbeat to {} failed: {}", url, e)),
    }
}

/// One inventory upload, returning the console's plain-text verdict.
fn sysinfo(url: &Url, body: &str, ca: Option<&http::CaBundle>) -> Result<String, String> {
    match http::post_json(url, SYSINFO_PATH, body, ca) {
        Ok(r) if r.ok() => Ok(r.body.trim().to_owned()),
        Ok(r) => Err(describe(SYSINFO_PATH, &r)),
        Err(e) => Err(format!("console: {} failed: {}", SYSINFO_PATH, e)),
    }
}

/// Send the inventory, and say plainly whether the console took it.
fn post_sysinfo(url: &Url, body: &str, ca: Option<&http::CaBundle>, fault: &mut Fault) {
    match sysinfo(url, body, ca) {
        Ok(answer) => {
            fault.clear("console: reachable again");
            match answer.as_str() {
                SYSINFO_OK => log::info!("console: inventory accepted -- the machine is in the device list"),
                SYSINFO_NO_ID => log::error!("console: inventory refused, no id was sent -- this is a bug"),
                // A device held for approval also answers SYSINFO_UPDATED, so
                // "accepted" is not "visible"; anything else is worth seeing.
                other => log::warn!("console: unexpected answer to {}: {:?}", SYSINFO_PATH, other),
            }
        }
        Err(msg) => fault.raise(&msg),
    }
}

/// A non-2xx reply, described so the cause is in the log rather than inferred.
///
/// A redirect is called out by name because it is the likeliest
/// misconfiguration here -- an http URL for a console that only serves https --
/// and the `Location` header is the entire diagnosis. This client does not
/// follow redirects: a 301 to https would need the TLS layer, and silently
/// following one across schemes is how a request ends up somewhere nobody
/// intended.
fn describe(path: &str, r: &http::Response) -> String {
    match (&r.location, r.status) {
        (Some(to), s) if s >= 300 && s < 400 => format!(
            "console: {} was redirected ({} -> {}). Redirects are not followed; \
             point --api-server at the final URL",
            path, s, to
        ),
        _ => {
            let body = r.body.trim();
            let excerpt: String = body.chars().take(200).collect();
            format!("console: {} answered {} {:?}", path, r.status, excerpt)
        }
    }
}

fn sysinfo_body(me: &Enrolment, uuid: &str, cpu: &str, memory: &str, os: &str) -> String {
    json::Object::new()
        .string("id", &me.id)
        .string("uuid", uuid)
        .string("cpu", cpu)
        .string("memory", memory)
        .string("os", os)
        .string("hostname", &me.hostname)
        .string("username", &me.username)
        // The same version the peer is told at handshake. One number, one
        // meaning: a console showing something the client does not see would
        // be worse than showing nothing.
        .string("version", crate::session::REPORTED_VERSION)
        .finish()
}

fn heartbeat_body(me: &Enrolment, uuid: &str) -> String {
    // `conns` is deliberately absent. It is the device's list of live incoming
    // connections, and the server reads an omitted field as "nothing is live
    // here" -- which is true of an agent that has no connection-id concept to
    // report. Sending a wrong list would close sessions; sending none only
    // leaves the console's Active view empty.
    json::Object::new().string("id", &me.id).string("uuid", uuid).finish()
}

/// A recurring fault, logged once at `warn` and thereafter at `debug`.
///
/// The agent runs unattended for weeks. A console that goes away overnight
/// would otherwise write 240 identical warnings an hour, and the first one --
/// the only one that says when it started -- scrolls out of reach. The same
/// reasoning as `clipboard`'s once-per-session note.
struct Fault {
    active: bool,
}

impl Fault {
    fn new() -> Self {
        Fault { active: false }
    }

    fn raise(&mut self, msg: &str) {
        if self.active {
            log::debug!("{}", msg);
        } else {
            log::warn!("{}", msg);
            self.active = true;
        }
    }

    fn clear(&mut self, msg: &str) {
        if self.active {
            log::info!("{}", msg);
            self.active = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn enrolment() -> Enrolment {
        Enrolment {
            api_server: "http://console.example.org:8080".to_owned(),
            id: "123456789".to_owned(),
            uuid: vec![0xde, 0xad, 0xbe, 0xef],
            hostname: "g5".to_owned(),
            username: "admin".to_owned(),
            ca_bundle: String::new(),
        }
    }

    /// Field names are the contract. A typo here is not a compile error and not
    /// a runtime error either -- it is a device that never appears, which is
    /// the failure this test exists to make loud.
    #[test]
    fn sysinfo_carries_every_field_the_console_reads() {
        let body = sysinfo_body(&enrolment(), "3q2+7w==", "PowerPC G5", "2 GB", "Mac OS X 10.5");
        for key in ["id", "uuid", "cpu", "memory", "os", "hostname", "username", "version"].iter() {
            assert!(json::field(&body, key).is_some(), "{} missing from {}", key, body);
        }
        assert_eq!(json::field(&body, "hostname"), Some("\"g5\""));
        assert_eq!(json::field(&body, "uuid"), Some("\"3q2+7w==\""));
    }

    /// The version the console shows and the version the peer negotiates
    /// against have to be the same number.
    #[test]
    fn sysinfo_reports_the_version_the_peer_is_told() {
        let body = sysinfo_body(&enrolment(), "u", "", "", "");
        let reported = format!("\"{}\"", crate::session::REPORTED_VERSION);
        assert_eq!(json::field(&body, "version"), Some(reported.as_str()));
    }

    #[test]
    fn a_heartbeat_is_just_the_identity() {
        let body = heartbeat_body(&enrolment(), "3q2+7w==");
        assert_eq!(body, "{\"id\":\"123456789\",\"uuid\":\"3q2+7w==\"}");
    }

    /// An omitted `conns` means "nothing is live here". Sending an empty array
    /// would mean the same, but sending the key at all invites someone to fill
    /// it in wrongly later.
    #[test]
    fn a_heartbeat_does_not_claim_any_live_connections() {
        assert_eq!(json::field(&heartbeat_body(&enrolment(), "u"), "conns"), None);
    }

    /// The reply the server actually sends when it does not know us.
    #[test]
    fn the_inventory_request_is_recognised() {
        assert!(json::field("{\"sysinfo\":1}", "sysinfo").map_or(false, json::truthy));
        assert!(!json::field("{}", "sysinfo").map_or(false, json::truthy));
        // A reply carrying only a strategy is not a request for the inventory.
        let strategy = "{\"modified_at\":9,\"strategy\":{\"sysinfo\":1}}";
        assert!(!json::field(strategy, "sysinfo").map_or(false, json::truthy));
    }

    /// A redirect is the likeliest misconfiguration, so its message has to name
    /// the destination rather than just the status.
    #[test]
    fn a_redirect_is_described_by_where_it_points() {
        let r = http::Response {
            status: 301,
            body: String::new(),
            location: Some("https://console.example.org/api/heartbeat".to_owned()),
        };
        let msg = describe(HEARTBEAT_PATH, &r);
        assert!(msg.contains("redirected"), "{}", msg);
        assert!(msg.contains("https://console.example.org/api/heartbeat"), "{}", msg);
    }

    #[test]
    fn other_failures_quote_the_body_but_not_all_of_it() {
        let r = http::Response { status: 500, body: "x".repeat(5000), location: None };
        let msg = describe(SYSINFO_PATH, &r);
        assert!(msg.contains("500"), "{}", msg);
        assert!(msg.len() < 400, "a 5000-byte error page must not land whole in the log");
    }

    /// The point of the type: one warning, then quiet, then a line saying it
    /// came back.
    #[test]
    fn a_recurring_fault_is_announced_once_and_its_recovery_once() {
        let mut f = Fault::new();
        assert!(!f.active);
        f.raise("down");
        assert!(f.active, "the first failure is the one that warns");
        f.raise("still down");
        assert!(f.active);
        f.clear("up");
        assert!(!f.active, "recovery re-arms the warning for next time");
        f.clear("up again");
        assert!(!f.active, "clearing something that was never raised says nothing");
    }

    #[test]
    fn a_url_that_does_not_parse_stops_the_thread_rather_than_the_agent() {
        // `serve` returns instead of looping; the assertion is that it returns
        // at all, which a panic or a retry loop would fail.
        let mut me = enrolment();
        me.api_server = "ftp://nope".to_owned();
        serve(me);
    }
}

#[cfg(test)]
mod console_tests {
    //! End to end against a stand-in console, over a real socket.
    //!
    //! This exercises `http` and `json` as well as this module, and covers the
    //! two things a test on the request body alone cannot see: that the
    //! inventory reply is plain text rather than JSON, and that a heartbeat
    //! from a machine the server does not know comes back asking for it.
    //!
    //! **It is a stand-in, not the real thing.** What it proves is that the
    //! agent's half matches the contract as read out of CortenDesk's
    //! `SyncController` -- not that the reading is right. Only the real
    //! container can settle that, and this is the check that makes running it
    //! worth doing once rather than debugging blind.

    use super::*;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::{Arc, Mutex};

    struct Request {
        path: String,
        head: String,
        body: String,
    }

    type Seen = Arc<Mutex<Vec<Request>>>;

    /// Serve exactly `count` requests, answering with `reply(path)`.
    fn spawn_console(count: usize, reply: fn(&str) -> String) -> (String, Seen) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", listener.local_addr().unwrap());
        let seen: Seen = Arc::new(Mutex::new(Vec::new()));
        let recorder = seen.clone();
        std::thread::spawn(move || {
            for _ in 0..count {
                let mut s = match listener.accept() {
                    Ok((s, _)) => s,
                    Err(_) => return,
                };
                let raw = read_request(&mut s);
                let (head, body) = split(&raw);
                let path = head
                    .lines()
                    .next()
                    .unwrap_or("")
                    .split_whitespace()
                    .nth(1)
                    .unwrap_or("")
                    .to_owned();
                let _ = s.write_all(reply(&path).as_bytes());
                let _ = s.flush();
                recorder.lock().unwrap().push(Request { path, head, body });
            }
        });
        (base, seen)
    }

    /// Read headers, then exactly the body they declare.
    fn read_request(s: &mut TcpStream) -> Vec<u8> {
        let mut raw = Vec::new();
        let mut buf = [0u8; 512];
        loop {
            let n = match s.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            };
            raw.extend_from_slice(&buf[..n]);
            if let Some(i) = blank_line(&raw) {
                let head = String::from_utf8_lossy(&raw[..i]).into_owned();
                if raw.len() >= i + 4 + declared_length(&head) {
                    break;
                }
            }
        }
        raw
    }

    fn blank_line(b: &[u8]) -> Option<usize> {
        if b.len() < 4 {
            return None;
        }
        (0..=b.len() - 4).find(|&i| &b[i..i + 4] == b"\r\n\r\n")
    }

    fn declared_length(head: &str) -> usize {
        head.lines()
            .find(|l| l.to_ascii_lowercase().starts_with("content-length:"))
            .and_then(|l| l.split(':').nth(1))
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(0)
    }

    fn split(raw: &[u8]) -> (String, String) {
        match blank_line(raw) {
            Some(i) => (
                String::from_utf8_lossy(&raw[..i]).into_owned(),
                String::from_utf8_lossy(&raw[i + 4..]).into_owned(),
            ),
            None => (String::from_utf8_lossy(raw).into_owned(), String::new()),
        }
    }

    fn http_200(body: &str) -> String {
        format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\
             Connection: close\r\n\r\n{}",
            body.len(),
            body
        )
    }

    /// A machine the console has never seen: the heartbeat is answered with a
    /// request for the inventory, and the inventory with the plain-text ack.
    fn unknown_device(path: &str) -> String {
        if path.ends_with("/heartbeat") {
            http_200("{\"sysinfo\":1}")
        } else {
            http_200(SYSINFO_OK)
        }
    }

    /// A machine already in the list: an empty object, and nothing to do.
    fn known_device(_path: &str) -> String {
        http_200("{}")
    }

    fn enrolment(api_server: &str) -> Enrolment {
        Enrolment {
            api_server: api_server.to_owned(),
            id: "123456789".to_owned(),
            uuid: vec![0xde, 0xad, 0xbe, 0xef],
            hostname: "g5".to_owned(),
            username: "admin".to_owned(),
            ca_bundle: String::new(),
        }
    }

    /// The whole first-contact sequence, which is what "the G5 appears in the
    /// console" actually consists of.
    #[test]
    fn an_unknown_machine_is_asked_for_its_inventory_and_accepted() {
        let (base, seen) = spawn_console(2, unknown_device);
        let url = Url::parse(&base).unwrap();
        let me = enrolment(&base);
        let uuid = crate::config::base64_encode(&me.uuid);

        assert_eq!(
            heartbeat(&url, &heartbeat_body(&me, &uuid), None),
            Ok(true),
            "an unrecognised machine must be asked to introduce itself"
        );
        let body = sysinfo_body(&me, &uuid, "PowerPC G5 @ 2 GHz (2 cores)", "2 GB", "Mac OS X 10.5");
        assert_eq!(sysinfo(&url, &body, None), Ok(SYSINFO_OK.to_owned()));

        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 2);
        assert_eq!(seen[0].path, "/api/heartbeat");
        assert_eq!(seen[1].path, "/api/sysinfo");
        assert_eq!(json::field(&seen[0].body, "id"), Some("\"123456789\""));
        assert_eq!(json::field(&seen[1].body, "cpu"), Some("\"PowerPC G5 @ 2 GHz (2 cores)\""));
        assert_eq!(json::field(&seen[1].body, "hostname"), Some("\"g5\""));
    }

    /// The id and uuid a heartbeat carries have to be the same pair the
    /// inventory registered, or the console pins one and ignores the other.
    #[test]
    fn the_heartbeat_and_the_inventory_agree_on_who_we_are() {
        let (base, seen) = spawn_console(2, unknown_device);
        let url = Url::parse(&base).unwrap();
        let me = enrolment(&base);
        let uuid = crate::config::base64_encode(&me.uuid);
        let _ = heartbeat(&url, &heartbeat_body(&me, &uuid), None);
        let _ = sysinfo(&url, &sysinfo_body(&me, &uuid, "", "", ""), None);

        let seen = seen.lock().unwrap();
        for key in ["id", "uuid"].iter() {
            assert_eq!(
                json::field(&seen[0].body, key),
                json::field(&seen[1].body, key),
                "{} differs between the heartbeat and the inventory",
                key
            );
        }
        assert_eq!(json::field(&seen[0].body, "uuid"), Some("\"3q2+7w==\""));
    }

    #[test]
    fn a_known_machine_is_not_asked_again() {
        let (base, _) = spawn_console(1, known_device);
        let url = Url::parse(&base).unwrap();
        let me = enrolment(&base);
        let uuid = crate::config::base64_encode(&me.uuid);
        assert_eq!(heartbeat(&url, &heartbeat_body(&me, &uuid), None), Ok(false));
    }

    /// Laravel reads a JSON body only when it is told the body is JSON, so the
    /// content type is load-bearing rather than decorative: with the wrong one
    /// every field arrives empty and the device silently never appears.
    #[test]
    fn the_request_is_well_formed_http() {
        let (base, seen) = spawn_console(1, known_device);
        let url = Url::parse(&base).unwrap();
        let me = enrolment(&base);
        let body = heartbeat_body(&me, &crate::config::base64_encode(&me.uuid));
        let _ = heartbeat(&url, &body, None);

        let seen = seen.lock().unwrap();
        let head = seen[0].head.to_ascii_lowercase();
        assert!(seen[0].head.starts_with("POST /api/heartbeat HTTP/1.1\r\n"), "{}", seen[0].head);
        assert!(head.contains("content-type: application/json"), "{}", seen[0].head);
        assert!(head.contains("host: 127.0.0.1:"), "{}", seen[0].head);
        assert!(head.contains("connection: close"), "{}", seen[0].head);
        assert!(
            head.contains(&format!("content-length: {}", body.len())),
            "the declared length has to match the body, or the server blocks waiting for the rest"
        );
        assert_eq!(seen[0].body, body);
    }

    /// A console mounted under a subdirectory by a reverse proxy.
    #[test]
    fn a_path_prefix_is_kept_in_front_of_the_endpoint() {
        let (base, seen) = spawn_console(1, known_device);
        let prefixed = format!("{}/console", base);
        let url = Url::parse(&prefixed).unwrap();
        let me = enrolment(&prefixed);
        let _ = heartbeat(&url, &heartbeat_body(&me, "u"), None);
        assert_eq!(seen.lock().unwrap()[0].path, "/console/api/heartbeat");
    }

    fn chunked_inventory_request(_path: &str) -> String {
        // The same body a proxy may deliver in pieces rather than whole.
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n\
         7\r\n{\"sysin\r\n6\r\nfo\":1}\r\n0\r\n\r\n"
            .to_owned()
    }

    /// A proxy may chunk the reply whatever we asked for; an undecoded body
    /// would read as "no inventory wanted" and the machine would never appear.
    #[test]
    fn a_chunked_reply_is_understood() {
        let (base, _) = spawn_console(1, chunked_inventory_request);
        let url = Url::parse(&base).unwrap();
        assert_eq!(heartbeat(&url, "{}", None), Ok(true));
    }

    fn server_error(_path: &str) -> String {
        let body = "{\"message\":\"Server Error\"}";
        format!(
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.len(),
            body
        )
    }

    /// A 500 must not read as "the console is happy and wants nothing".
    #[test]
    fn a_server_error_is_a_failure_not_a_quiet_success() {
        let (base, _) = spawn_console(2, server_error);
        let url = Url::parse(&base).unwrap();
        let beat = heartbeat(&url, "{}", None);
        assert!(beat.is_err(), "500 must not parse as a successful heartbeat");
        assert!(beat.unwrap_err().contains("500"));
        assert!(sysinfo(&url, "{}", None).is_err());
    }

    /// Nothing listening is the ordinary case -- the console is down, or the
    /// URL is wrong -- and it has to be an error rather than a hang or a panic.
    #[test]
    fn a_console_that_is_not_there_fails_promptly() {
        // Bind and drop, so the port is one nothing is listening on.
        let dead = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap();
        let url = Url::parse(&format!("http://{}", dead)).unwrap();
        assert!(heartbeat(&url, "{}", None).is_err());
    }
}
