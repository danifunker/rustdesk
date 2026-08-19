//! A blocking HTTP/1.1 client, enough to POST JSON at a console API.
//!
//! Mirrors what upstream's `src/hbbs_http/` does with reqwest on tokio. Neither
//! is available here -- reqwest alone is most of a dependency graph, and tokio
//! is the thing this whole crate exists to avoid (see `Cargo.toml`). What the
//! agent actually needs is one request shape: POST a small JSON body, read a
//! small reply, close. That is what this is, and nothing more.
//!
//! # Deliberately one request per connection
//!
//! Every request sends `Connection: close` and reads the body to EOF. Keep-alive
//! would save a TCP handshake every fifteen seconds and cost a pool, an idle
//! timeout, and a class of bug where a half-closed socket is discovered only on
//! the next write. The agent sends four requests a minute; the handshake is not
//! worth that.
//!
//! Chunked bodies are still decoded even so. A reverse proxy in front of the
//! console may chunk a reply regardless of what we asked for, and the failure
//! would otherwise be a body with a hex length glued to the front of it --
//! which parses as neither JSON nor an error.
//!
//! # TLS
//!
//! [`Conn`] has two arms and everything above them is transport-agnostic: an
//! https URL is parsed, routed, framed and reported on exactly like an http
//! one, and the only difference is which arm `connect` returns. The session
//! itself is mbedTLS, through `src/tls_shim.c`; see that file for why it is C
//! and not a Rust TLS stack.
//!
//! Two things about it belong here rather than there. **The CA bundle is
//! explicit** -- see [`CaBundle`] -- because this platform's trust store is a
//! decade stale and trusting it would fail on most of the public internet.
//! And **a truncated body is an error**: over TLS a close with no close_notify
//! is indistinguishable from a complete reply at the socket, so `parse_response`
//! checks what arrived against `Content-Length` and the shim defers to it.

use std::io::{self, ErrorKind, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

/// Long enough for a busy console behind a proxy, short enough that a wedged
/// server cannot outlive the heartbeat interval that drives it.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const IO_TIMEOUT: Duration = Duration::from_secs(10);

/// A reply larger than this is not one of ours. The bodies this reads are a few
/// hundred bytes; the cap exists so a misrouted URL returning a large page
/// cannot be read into memory on a machine with 512 MB of it.
const MAX_RESPONSE: usize = 256 * 1024;

/// Where the console lives: scheme, authority, and any path the console is
/// mounted under.
#[derive(Clone, Debug, PartialEq)]
pub struct Url {
    pub secure: bool,
    pub host: String,
    pub port: u16,
    /// Path prefix, without a trailing slash. Empty for a console at the root,
    /// `/rustdesk` for one behind a proxy that mounts it in a subdirectory.
    pub prefix: String,
}

impl Url {
    /// Parse a base URL.
    ///
    /// **A missing scheme means https.** That is the safe direction to guess:
    /// assuming http would silently downgrade a console that was reachable over
    /// TLS, and the agent would have no way to notice. Someone who means plain
    /// http can say so, and has to.
    pub fn parse(s: &str) -> Result<Url, String> {
        let s = s.trim();
        if s.is_empty() {
            return Err("empty URL".to_owned());
        }
        let (secure, rest) = match s.find("://") {
            Some(i) => {
                let scheme = s[..i].to_ascii_lowercase();
                let rest = &s[i + 3..];
                match scheme.as_str() {
                    "https" => (true, rest),
                    "http" => (false, rest),
                    other => return Err(format!("unsupported scheme {:?}: expected http or https", other)),
                }
            }
            None => (true, s),
        };
        let (authority, prefix) = match rest.find('/') {
            Some(i) => (&rest[..i], rest[i..].trim_end_matches('/').to_owned()),
            None => (rest, String::new()),
        };
        if authority.is_empty() {
            return Err("no host in URL".to_owned());
        }
        // An IPv6 literal is bracketed, and the colons inside it are not a port
        // separator. Rare here, but the failure would be a nonsense hostname
        // rather than an error, which is the kind that wastes an afternoon.
        let (host, port_text) = if authority.starts_with('[') {
            let close = authority.find(']').ok_or_else(|| "unterminated IPv6 literal".to_owned())?;
            let host = authority[..close + 1].to_owned();
            match authority[close + 1..].strip_prefix(':') {
                Some(p) => (host, Some(p)),
                None => (host, None),
            }
        } else {
            match authority.rfind(':') {
                Some(i) => (authority[..i].to_owned(), Some(&authority[i + 1..])),
                None => (authority.to_owned(), None),
            }
        };
        if host.is_empty() {
            return Err("no host in URL".to_owned());
        }
        let port = match port_text {
            Some(p) => p.parse::<u16>().map_err(|_| format!("bad port {:?}", p))?,
            None if secure => 443,
            None => 80,
        };
        Ok(Url { secure, host, port, prefix })
    }

    /// What goes in the `Host` header: the port is included only when it is not
    /// the default for the scheme, which is what every other client does and
    /// what a virtual-host config expects to match.
    fn host_header(&self) -> String {
        let default = if self.secure { 443 } else { 80 };
        if self.port == default {
            self.host.clone()
        } else {
            format!("{}:{}", self.host, self.port)
        }
    }
}

impl std::fmt::Display for Url {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(
            f,
            "{}://{}{}",
            if self.secure { "https" } else { "http" },
            self.host_header(),
            self.prefix
        )
    }
}

#[derive(Debug)]
pub struct Response {
    pub status: u16,
    pub body: String,
    /// `Location`, when the server sent one. Kept because a redirect is the
    /// most likely misconfiguration here and the header is the whole diagnosis.
    pub location: Option<String>,
}

impl Response {
    pub fn ok(&self) -> bool {
        self.status >= 200 && self.status < 300
    }
}

/// The certificates an https console is verified against.
///
/// **The system trust store is not one of the options, on purpose.** Leopard's
/// roots are a decade stale -- DST Root CA X3 expired in 2021 and ISRG Root X1
/// was never there -- so anything behind Let's Encrypt fails against it however
/// modern the TLS library is. This is the difference between "works on my
/// machine" and "works on the G5", and it is why the bundle is a first-class
/// setting rather than something inferred.
pub struct CaBundle {
    /// PEM, **NUL-terminated**: mbedTLS counts the NUL in the length it is
    /// given, and a length that omits it fails to parse with no useful error.
    pem: Vec<u8>,
    /// Where it came from, for the log. A verification failure is usually a
    /// question about which bundle was actually loaded.
    pub source: String,
}

/// Shows where it came from and how much of it there is -- never the
/// certificates themselves, which would bury any message that printed one.
impl std::fmt::Debug for CaBundle {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "CaBundle({}, {} bytes)", self.source, self.pem.len())
    }
}

impl CaBundle {
    /// Where a bundle is looked for when none was named, in order.
    ///
    /// **Beside the agent first**, because that is the one place it is always
    /// installed: `bundle.sh` puts a copy in the `.app`, and `install.sh`
    /// copies it out next to the binary. Both land as a sibling of the
    /// executable -- inside the bundle the agent lives in `Contents/Resources`,
    /// not `Contents/MacOS`, so there is no `../Resources` hop to make.
    ///
    /// Then MacPorts, where `curl-ca-bundle` puts one, then the usual Unix
    /// paths so a host build has something to find.
    pub fn search_paths() -> Vec<std::path::PathBuf> {
        let mut v = Vec::new();
        if let Ok(exe) = std::env::current_exe() {
            if let Some(dir) = exe.parent() {
                v.push(dir.join("cacert.pem"));
            }
        }
        for p in [
            "/opt/local/share/curl/curl-ca-bundle.crt",
            "/etc/ssl/cert.pem",
            "/etc/ssl/certs/ca-certificates.crt",
            "/usr/local/etc/openssl/cert.pem",
        ]
        .iter()
        {
            v.push(std::path::PathBuf::from(p));
        }
        v
    }

    /// Load `explicit` if it names a file, otherwise the first of
    /// [`search_paths`](Self::search_paths) that exists.
    pub fn load(explicit: &str) -> io::Result<CaBundle> {
        let read = |p: &std::path::Path| -> io::Result<CaBundle> {
            // The path is put back into the error: `std::fs` drops it, and
            // "No such file or directory" on its own is useless when the whole
            // question is which file was meant.
            let mut pem = std::fs::read(p)
                .map_err(|e| io::Error::new(e.kind(), format!("{}: {}", p.display(), e)))?;
            // Caught here rather than at the handshake, where it would surface
            // as an unhelpful parse error. Pointing this at a private key or a
            // proxy's HTML error page is an easy mistake to make.
            if !contains(&pem, b"-----BEGIN CERTIFICATE-----") {
                return Err(io::Error::new(
                    ErrorKind::InvalidData,
                    format!("{} holds no PEM certificates", p.display()),
                ));
            }
            pem.push(0);
            Ok(CaBundle { pem, source: p.display().to_string() })
        };
        if !explicit.is_empty() {
            return read(std::path::Path::new(explicit));
        }
        let paths = Self::search_paths();
        for p in &paths {
            if p.is_file() {
                return read(p);
            }
        }
        let tried: Vec<String> = paths.iter().map(|p| p.display().to_string()).collect();
        Err(io::Error::new(
            ErrorKind::NotFound,
            format!(
                "no CA bundle found, so an https console cannot be verified. \
                 Set one with --ca-bundle, or install one where it is looked \
                 for: {}",
                tried.join(", ")
            ),
        ))
    }
}

/// One connection, whatever it is carrying.
enum Conn {
    Plain(TcpStream),
    #[cfg(not(no_tls))]
    Tls(tls::Tls),
}

impl Read for Conn {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Conn::Plain(s) => s.read(buf),
            #[cfg(not(no_tls))]
            Conn::Tls(s) => s.read(buf),
        }
    }
}

impl Write for Conn {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Conn::Plain(s) => s.write(buf),
            #[cfg(not(no_tls))]
            Conn::Tls(s) => s.write(buf),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Conn::Plain(s) => s.flush(),
            #[cfg(not(no_tls))]
            Conn::Tls(s) => s.flush(),
        }
    }
}

/// Open the socket, and wrap it in TLS if the URL asked for it.
fn connect(url: &Url, ca: Option<&CaBundle>) -> io::Result<Conn> {
    // Try every address the name resolves to rather than only the first: a
    // console with an AAAA record on a Mac with no working IPv6 route is a
    // connect timeout, not a resolution failure, and trying the next address
    // is what turns that into a working agent.
    let addrs = (url.host.as_str(), url.port).to_socket_addrs()?;
    let mut last = io::Error::new(ErrorKind::AddrNotAvailable, format!("{} resolved to nothing", url.host));
    let mut sock = None;
    for addr in addrs {
        match TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT) {
            Ok(s) => {
                s.set_read_timeout(Some(IO_TIMEOUT))?;
                s.set_write_timeout(Some(IO_TIMEOUT))?;
                // Small bodies, one write: Nagle would only add latency.
                let _ = s.set_nodelay(true);
                sock = Some(s);
                break;
            }
            Err(e) => last = e,
        }
    }
    let stream = match sock {
        Some(s) => s,
        None => return Err(last),
    };
    if !url.secure {
        return Ok(Conn::Plain(stream));
    }
    tls_wrap(stream, url, ca)
}

#[cfg(no_tls)]
fn tls_wrap(_stream: TcpStream, _url: &Url, _ca: Option<&CaBundle>) -> io::Result<Conn> {
    Err(io::Error::new(
        ErrorKind::Other,
        "https is not available: this agent was built without mbedTLS. Rebuild \
         with the mbedtls3 port installed, or use the console's plain-http port",
    ))
}

#[cfg(not(no_tls))]
fn tls_wrap(stream: TcpStream, url: &Url, ca: Option<&CaBundle>) -> io::Result<Conn> {
    let ca = ca.ok_or_else(|| {
        io::Error::new(ErrorKind::InvalidInput, "an https console needs a CA bundle")
    })?;
    // An IPv6 literal is bracketed in a URL and not in a certificate.
    let host = url.host.trim_start_matches('[').trim_end_matches(']');
    tls::Tls::connect(stream, host, ca).map(Conn::Tls)
}

/// POST `body` as JSON to `url` + `path`, and read the reply.
///
/// `ca` is required for an https URL and ignored for an http one.
pub fn post_json(url: &Url, path: &str, body: &str, ca: Option<&CaBundle>) -> io::Result<Response> {
    let mut conn = connect(url, ca)?;
    let request = format!(
        "POST {}{} HTTP/1.1\r\n\
         Host: {}\r\n\
         User-Agent: rustdesk-ppc-agent/{}\r\n\
         Content-Type: application/json\r\n\
         Accept: application/json\r\n\
         Content-Length: {}\r\n\
         Connection: close\r\n\
         \r\n\
         {}",
        url.prefix,
        path,
        url.host_header(),
        env!("CARGO_PKG_VERSION"),
        body.len(),
        body
    );
    conn.write_all(request.as_bytes())?;
    conn.flush()?;

    let mut raw = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        let n = conn.read(&mut buf)?;
        if n == 0 {
            break;
        }
        raw.extend_from_slice(&buf[..n]);
        if raw.len() > MAX_RESPONSE {
            return Err(io::Error::new(
                ErrorKind::InvalidData,
                format!("reply larger than {} bytes -- is this the console?", MAX_RESPONSE),
            ));
        }
    }
    parse_response(&raw)
}

/// Split a raw reply into a status, a decoded body and any `Location`.
///
/// Separate from the socket so every shape a proxy can produce is testable on
/// a host with no network at all.
fn parse_response(raw: &[u8]) -> io::Result<Response> {
    let split = find(raw, b"\r\n\r\n")
        .ok_or_else(|| io::Error::new(ErrorKind::InvalidData, "no header/body separator in reply"))?;
    let head = String::from_utf8_lossy(&raw[..split]).into_owned();
    let body_bytes = &raw[split + 4..];

    let mut lines = head.split("\r\n");
    let status_line = lines
        .next()
        .ok_or_else(|| io::Error::new(ErrorKind::InvalidData, "empty reply"))?;
    // "HTTP/1.1 200 OK" -- the code is the second field.
    let status: u16 = status_line
        .split_whitespace()
        .nth(1)
        .and_then(|c| c.parse().ok())
        .ok_or_else(|| {
            io::Error::new(ErrorKind::InvalidData, format!("not an HTTP reply: {:?}", status_line))
        })?;

    let mut chunked = false;
    let mut length: Option<usize> = None;
    let mut location = None;
    for line in lines {
        let (name, value) = match line.find(':') {
            Some(i) => (line[..i].trim().to_ascii_lowercase(), line[i + 1..].trim()),
            None => continue,
        };
        match name.as_str() {
            "transfer-encoding" => chunked = value.to_ascii_lowercase().contains("chunked"),
            "content-length" => length = value.parse().ok(),
            "location" => location = Some(value.to_owned()),
            _ => {}
        }
    }

    let body = if chunked {
        dechunk(body_bytes)?
    } else {
        match length {
            Some(n) if n <= body_bytes.len() => body_bytes[..n].to_vec(),
            // Short of what was declared means the connection died mid-reply.
            // Reported rather than returned as a partial body, because over
            // TLS a truncated close is otherwise indistinguishable from a
            // complete one -- see `rd_tls_read`, which defers exactly this
            // check to here.
            Some(n) => {
                return Err(io::Error::new(
                    ErrorKind::UnexpectedEof,
                    format!("reply truncated: {} bytes declared, {} arrived", n, body_bytes.len()),
                ))
            }
            None => body_bytes.to_vec(),
        }
    };
    Ok(Response { status, body: String::from_utf8_lossy(&body).into_owned(), location })
}

/// Reassemble a `Transfer-Encoding: chunked` body.
fn dechunk(mut b: &[u8]) -> io::Result<Vec<u8>> {
    let bad = || io::Error::new(ErrorKind::InvalidData, "malformed chunked body");
    let mut out = Vec::new();
    loop {
        let eol = find(b, b"\r\n").ok_or_else(bad)?;
        // The size line may carry chunk extensions after a semicolon.
        let line = String::from_utf8_lossy(&b[..eol]).into_owned();
        let size_text = line.split(';').next().unwrap_or("").trim();
        let size = usize::from_str_radix(size_text, 16).map_err(|_| bad())?;
        b = &b[eol + 2..];
        if size == 0 {
            return Ok(out);
        }
        if size > b.len() {
            return Err(bad());
        }
        out.extend_from_slice(&b[..size]);
        b = &b[size..];
        // Each chunk is followed by its own CRLF.
        if b.starts_with(b"\r\n") {
            b = &b[2..];
        }
    }
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    find(haystack, needle).is_some()
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    (0..=haystack.len() - needle.len()).find(|&i| &haystack[i..i + needle.len()] == needle)
}


/// The mbedTLS shim in `src/tls_shim.c`.
///
/// Compiled out entirely when mbedTLS was not found at build time, so a
/// checkout on a machine that only needs the LAN path still builds -- see
/// `build.rs`. `tls_wrap` is the one place that difference is visible.
#[cfg(not(no_tls))]
mod tls {
    use std::ffi::CString;
    use std::io::{self, ErrorKind, Read, Write};
    use std::net::TcpStream;
    use std::os::raw::{c_char, c_int, c_void};
    use std::os::unix::io::AsRawFd;

    use super::CaBundle;

    extern "C" {
        fn rd_tls_connect(
            fd: c_int,
            hostname: *const c_char,
            ca_pem: *const u8,
            ca_len: usize,
            err: *mut c_char,
            errlen: usize,
        ) -> *mut c_void;
        fn rd_tls_read(t: *mut c_void, buf: *mut u8, len: usize) -> c_int;
        fn rd_tls_write(t: *mut c_void, buf: *const u8, len: usize) -> c_int;
        fn rd_tls_version(t: *mut c_void) -> *const c_char;
        fn rd_tls_ciphersuite(t: *mut c_void) -> *const c_char;
        fn rd_tls_strerror(ret: c_int, buf: *mut c_char, len: usize);
        fn rd_tls_free(t: *mut c_void);
    }

    /// A TLS session over a socket it owns.
    ///
    /// The `TcpStream` is held because the shim keeps its file descriptor:
    /// dropping the stream first would leave mbedTLS reading a closed one. It
    /// is never touched from Rust again, hence the underscore.
    pub struct Tls {
        handle: *mut c_void,
        _stream: TcpStream,
    }

    impl Tls {
        pub fn connect(stream: TcpStream, hostname: &str, ca: &CaBundle) -> io::Result<Tls> {
            let host = CString::new(hostname).map_err(|_| {
                io::Error::new(ErrorKind::InvalidInput, "the host name contains a NUL")
            })?;
            let mut err = [0 as c_char; 320];
            let handle = unsafe {
                rd_tls_connect(
                    stream.as_raw_fd(),
                    host.as_ptr(),
                    ca.pem.as_ptr(),
                    ca.pem.len(),
                    err.as_mut_ptr(),
                    err.len(),
                )
            };
            if handle.is_null() {
                return Err(io::Error::new(ErrorKind::Other, unsafe { text(err.as_ptr()) }));
            }
            let t = Tls { handle, _stream: stream };
            // At debug rather than info: it is one line per request, and the
            // question it answers ("did TLS agree on anything?") is only asked
            // when something is wrong.
            log::debug!("tls: {}, {}", t.version(), t.ciphersuite());
            Ok(t)
        }

        pub fn version(&self) -> String {
            unsafe { text(rd_tls_version(self.handle)) }
        }

        pub fn ciphersuite(&self) -> String {
            unsafe { text(rd_tls_ciphersuite(self.handle)) }
        }
    }

    impl Read for Tls {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            let n = unsafe { rd_tls_read(self.handle, buf.as_mut_ptr(), buf.len()) };
            if n >= 0 {
                Ok(n as usize)
            } else {
                Err(error("TLS read failed", n))
            }
        }
    }

    impl Write for Tls {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            let n = unsafe { rd_tls_write(self.handle, buf.as_ptr(), buf.len()) };
            if n >= 0 {
                Ok(n as usize)
            } else {
                Err(error("TLS write failed", n))
            }
        }
        /// Nothing to flush: `mbedtls_ssl_write` hands each record to the
        /// socket as it is produced.
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    impl Drop for Tls {
        fn drop(&mut self) {
            unsafe { rd_tls_free(self.handle) }
        }
    }

    fn error(what: &str, ret: c_int) -> io::Error {
        let mut buf = [0 as c_char; 160];
        unsafe { rd_tls_strerror(ret, buf.as_mut_ptr(), buf.len()) };
        io::Error::new(ErrorKind::Other, format!("{}: {}", what, unsafe { text(buf.as_ptr()) }))
    }

    /// A NUL-terminated C string as an owned `String`. Lossy on purpose: this
    /// is only ever a diagnostic, and mangling one is better than dropping it.
    unsafe fn text(p: *const c_char) -> String {
        if p.is_null() {
            return String::new();
        }
        std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn urls_split_into_scheme_host_port_and_prefix() {
        let u = Url::parse("http://console.example.org:8080").unwrap();
        assert_eq!((u.secure, u.host.as_str(), u.port, u.prefix.as_str()), (false, "console.example.org", 8080, ""));

        let u = Url::parse("https://console.example.org").unwrap();
        assert_eq!((u.secure, u.port), (true, 443));

        let u = Url::parse("http://console.example.org").unwrap();
        assert_eq!(u.port, 80);

        let u = Url::parse("https://example.org/rustdesk/").unwrap();
        assert_eq!(u.prefix, "/rustdesk", "a trailing slash would double up against the path");
    }

    /// The guess has to be the one that cannot silently downgrade a TLS console.
    #[test]
    fn a_missing_scheme_means_https() {
        let u = Url::parse("console.example.org").unwrap();
        assert!(u.secure);
        assert_eq!(u.port, 443);
    }

    #[test]
    fn ipv6_literals_keep_their_colons() {
        let u = Url::parse("http://[2001:db8::1]:8080/x").unwrap();
        assert_eq!((u.host.as_str(), u.port, u.prefix.as_str()), ("[2001:db8::1]", 8080, "/x"));
        let u = Url::parse("http://[2001:db8::1]").unwrap();
        assert_eq!((u.host.as_str(), u.port), ("[2001:db8::1]", 80));
    }

    #[test]
    fn nonsense_urls_are_rejected_with_a_reason() {
        for bad in ["", "   ", "ftp://example.org", "http://", "http://example.org:notaport"].iter() {
            assert!(Url::parse(bad).is_err(), "{:?} should not parse", bad);
        }
    }

    /// The port belongs in `Host` only when it is not the scheme's default,
    /// which is what a virtual-host config matches against.
    #[test]
    fn the_host_header_omits_a_default_port() {
        assert_eq!(Url::parse("https://a.example").unwrap().host_header(), "a.example");
        assert_eq!(Url::parse("http://a.example").unwrap().host_header(), "a.example");
        assert_eq!(Url::parse("http://a.example:8080").unwrap().host_header(), "a.example:8080");
    }

    #[test]
    fn a_plain_reply_is_split_at_the_blank_line() {
        let raw = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n{\"sysinfo\":1}\r\n";
        let r = parse_response(raw).unwrap();
        assert_eq!(r.status, 200);
        assert!(r.ok());
        assert_eq!(r.body, "{\"sysinfo\":1}\r\n");
    }

    /// A body longer than `Content-Length` is truncated to it; the extra is a
    /// proxy artefact, not ours.
    #[test]
    fn content_length_bounds_the_body() {
        let raw = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}trailing garbage";
        assert_eq!(parse_response(raw).unwrap().body, "{}");
    }

    /// A proxy may chunk a reply whatever we asked for, and an undecoded body
    /// parses as neither JSON nor an error.
    #[test]
    fn chunked_bodies_are_reassembled() {
        let raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n7\r\n{\"a\":1,\r\n5\r\n\"b\":2\r\n1\r\n}\r\n0\r\n\r\n";
        let r = parse_response(raw).unwrap();
        assert_eq!(r.body, "{\"a\":1,\"b\":2}");
    }

    #[test]
    fn chunk_extensions_are_ignored() {
        let raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;foo=bar\r\n{}\r\n0\r\n\r\n";
        assert_eq!(parse_response(raw).unwrap().body, "{}");
    }

    /// Header names are case-insensitive and a proxy will not match our casing.
    #[test]
    fn header_names_are_matched_case_insensitively() {
        let raw = b"HTTP/1.1 301 Moved\r\nLOCATION: https://a.example/x\r\nCONTENT-LENGTH: 0\r\n\r\n";
        let r = parse_response(raw).unwrap();
        assert_eq!(r.status, 301);
        assert!(!r.ok());
        assert_eq!(r.location.as_deref(), Some("https://a.example/x"));
    }

    #[test]
    fn a_reply_that_is_not_http_is_an_error_not_a_panic() {
        assert!(parse_response(b"garbage with no separator").is_err());
        assert!(parse_response(b"garbage\r\n\r\nbody").is_err());
        assert!(parse_response(b"").is_err());
    }

    /// An https URL parses and prints like any other; only `connect` differs.
    #[test]
    fn an_https_url_is_ordinary_until_it_is_dialled() {
        let u = Url::parse("https://console.example.org").unwrap();
        assert_eq!(u.to_string(), "https://console.example.org");
        assert!(u.secure);
    }

    /// A socket that has been opened but has no certificates to verify against
    /// must fail rather than fall back to plaintext. Driven against a local
    /// listener so nothing here touches the network.
    #[test]
    fn https_without_a_ca_bundle_never_falls_back_to_plaintext() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        std::thread::spawn(move || {
            let _ = listener.accept();
        });
        let stream = TcpStream::connect(addr).unwrap();
        let url = Url::parse(&format!("https://{}", addr)).unwrap();
        match tls_wrap(stream, &url, None) {
            Ok(_) => panic!("https must not proceed without certificates"),
            Err(e) => {
                let m = e.to_string();
                // Which of the two depends on whether mbedTLS was present at
                // build time; both are a refusal, which is the point.
                assert!(
                    m.contains("CA bundle") || m.contains("without mbedTLS"),
                    "{}",
                    m
                );
            }
        }
    }

    /// Pointing the bundle at the wrong file is an easy mistake, and the error
    /// has to name the file rather than surface later as a handshake failure.
    #[test]
    fn a_ca_bundle_that_holds_no_certificates_is_rejected_by_name() {
        let mut path = std::env::temp_dir();
        path.push(format!("rdppc-ca-{}.pem", std::process::id()));
        std::fs::write(&path, b"-----BEGIN PRIVATE KEY-----\nnope\n").unwrap();
        let e = CaBundle::load(path.to_str().unwrap()).unwrap_err();
        assert!(e.to_string().contains("no PEM certificates"), "{}", e);
        assert!(e.to_string().contains("rdppc-ca-"), "the message must name the file: {}", e);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_ca_bundle_that_is_not_there_says_where_it_looked() {
        let e = CaBundle::load("/nonexistent/definitely/not/here.pem").unwrap_err();
        assert!(e.to_string().len() > 0);
        assert!(!CaBundle::search_paths().is_empty());
    }

    /// A body shorter than the declared length is a dead connection, not a
    /// short reply -- and over TLS it is the only sign of a truncated close.
    #[test]
    fn a_truncated_body_is_an_error() {
        let raw = b"HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n{}";
        let e = parse_response(raw).unwrap_err();
        assert_eq!(e.kind(), ErrorKind::UnexpectedEof);
        assert!(e.to_string().contains("truncated"), "{}", e);
    }

    #[test]
    fn find_locates_a_subsequence() {
        assert_eq!(find(b"abcdef", b"cd"), Some(2));
        assert_eq!(find(b"abcdef", b"xy"), None);
        assert_eq!(find(b"ab", b"abcdef"), None);
        assert_eq!(find(b"abc", b""), None);
    }
}
