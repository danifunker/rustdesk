//! Run the agent's portable modules on the target and say what actually works.
//!
//! Cross-compiling proves the code typechecks for IRIX; it proves nothing about
//! run time, and on this platform that gap is where the failures live —
//! libsodium's `sodium_init()` returned -1 on IRIX while every `crypto_*` call
//! worked, which is the easiest possible thing to ship broken.
//!
//! So each check exercises a real code path and prints a verdict. Nothing here
//! needs a display, a peer, or the rendezvous server; the one network test
//! talks to a plain HTTP server on the build host.

use rustdesk_ppc_agent::{config, convert, crypto, http, json, png, zstd_frame};

fn head(s: &str) {
    println!("\n--- {} ---", s);
}

fn ok(name: &str, good: bool, detail: &str) {
    println!("  [{}] {:<28} {}", if good { "PASS" } else { "FAIL" }, name, detail);
}

/// A small synthetic screen: memory order A,B,G,R, which is what IRIX's
/// ReadDisplay hands back. Deliberately not the Mac's A,R,G,B — the converters
/// are the place where that difference bites.
fn synthetic_abgr(w: usize, h: usize) -> Vec<u8> {
    let mut v = vec![0u8; w * h * 4];
    for y in 0..h {
        for x in 0..w {
            let i = (y * w + x) * 4;
            v[i] = 0xff;                       // A / unused
            v[i + 1] = (x * 255 / w.max(1)) as u8; // B
            v[i + 2] = (y * 255 / h.max(1)) as u8; // G
            v[i + 3] = 0x40;                   // R
        }
    }
    v
}

fn main() {
    println!("agent-portable self-test on {}", std::env::consts::OS);
    println!("pointer width {} bits, endian {}",
             std::mem::size_of::<usize>() * 8,
             if cfg!(target_endian = "big") { "big" } else { "little" });

    head("json");
    {
        let doc = json::Object::new()
            .string("id", "IRIS-0001")
            .number("port", 21116)
            .finish();
        // `field` returns the value exactly as it appears in the document, so a
        // string value arrives still quoted; `escape_into` likewise writes the
        // surrounding quotes. Asserting the unquoted forms was a bug in this
        // test, not in the module.
        let id = json::field(&doc, "id");
        let port = json::field(&doc, "port");
        ok("build + field", id == Some("\"IRIS-0001\"") && port == Some("21116"),
           &format!("{:?} port={:?}", id, port));
        let mut esc = String::new();
        json::escape_into("a\"b\\c\nd", &mut esc);
        ok("escape", esc == "\"a\\\"b\\\\c\\nd\"", &esc);
        ok("truthy", json::truthy("true") && !json::truthy("0"), "true/0");
    }

    head("convert (ARGB -> I420)");
    {
        let (w, h) = (64usize, 32usize);
        let fb = synthetic_abgr(w, h);
        let mut a = convert::I420::new(w, h);
        let mut b = convert::I420::new(w, h);
        convert::argb_to_i420_rows(&fb, w * 4, &mut a, 0, h);
        convert::argb_to_i420_rows_rust(&fb, w * 4, &mut b, 0, h);
        let same_y = a.y == b.y;
        let same_u = a.u == b.u;
        let same_v = a.v == b.v;
        ok("C shim == Rust path", same_y && same_u && same_v,
           &format!("y {} u {} v {}", same_y, same_u, same_v));
        let ymin = *a.y.iter().min().unwrap_or(&0);
        let ymax = *a.y.iter().max().unwrap_or(&0);
        ok("luma has range", ymax > ymin, &format!("Y in {}..{}", ymin, ymax));
    }

    head("png");
    {
        let (w, h) = (32usize, 16usize);
        let fb = synthetic_abgr(w, h);
        let packed = png::pack_rgb_rows(&fb, w * 4, w, h);
        let packed_rust = png::pack_rgb_rows_rust(&fb, w * 4, w, h);
        ok("pack C == pack Rust", packed == packed_rust,
           &format!("{} bytes", packed.len()));
        match png::encode_argb(&fb, w * 4, w, h) {
            Ok(bytes) => {
                let sig_ok = bytes.len() > 8 && &bytes[..8] == b"\x89PNG\r\n\x1a\n";
                ok("encode_argb (zlib)", sig_ok,
                   &format!("{} bytes, signature {}", bytes.len(), if sig_ok { "ok" } else { "BAD" }));
            }
            Err(e) => ok("encode_argb (zlib)", false, e),
        }
    }

    head("zstd_frame");
    {
        let raw = zstd_frame::raw_frame(b"the quick brown fox");
        // 0xFD2FB528 little-endian, so 28 b5 2f fd in memory. Worth asserting
        // on a big-endian target specifically: a to_le_bytes that was really a
        // native-endian write would pass on the G5's little sibling and fail here.
        let magic_ok = raw.len() > 4 && raw[0] == 0x28 && raw[1] == 0xb5
                       && raw[2] == 0x2f && raw[3] == 0xfd;
        ok("raw_frame magic", magic_ok, &format!("{} bytes", raw.len()));
    }

    head("crypto (libsodium)");
    {
        // sodium_init is the call that returned -1 on IRIX from C while the
        // primitives all worked; sodiumoxide calls it inside init().
        let init = sodiumoxide::init();
        ok("sodiumoxide::init", init.is_ok(), &format!("{:?}", init));

        // The real handshake path: sign an IdPk with the combined-form
        // sign::sign, exactly as the agent does when it registers.
        let (pk, sk) = sodiumoxide::crypto::sign::gen_keypair();
        let hs = crypto::Handshake::new();
        let signed = hs.signed_id("IRIS-0001", &sk);
        ok("signed_id (sign+protobuf)", !signed.is_empty(),
           &format!("{} bytes", signed.len()));
        let opened = sodiumoxide::crypto::sign::verify(&signed, &pk);
        match &opened {
            Ok(v) => ok("sign::verify", true, &format!("recovered {} bytes", v.len())),
            Err(_) => ok("sign::verify", false, "verify failed"),
        }

        let key = sodiumoxide::crypto::secretbox::gen_key();
        let mut enc = crypto::SecureChannel::new(key.clone());
        let mut dec = crypto::SecureChannel::new(key);
        let ct = enc.seal(b"hello irix");
        match dec.open(&ct) {
            Ok(pt) => ok("secretbox seal/open", pt == b"hello irix",
                         &format!("{} -> {} bytes", ct.len(), pt.len())),
            Err(_) => ok("secretbox seal/open", false, "open failed"),
        }

        let salt = b"0123456789abcdef";
        let challenge = b"fedcba9876543210";
        let a = crypto::expected_login_hash("hunter2", salt, challenge);
        let b = crypto::expected_login_hash("hunter2", salt, challenge);
        ok("login hash (sha2)", a == b && !a.is_empty(), &format!("{} bytes", a.len()));
        ok("verify_login_hash", crypto::verify_login_hash(&a, &b), "matching pair");
    }

    head("config");
    {
        let path = std::path::PathBuf::from("/tmp/agent-portable-selftest.toml");
        let _ = std::fs::remove_file(&path);
        let mut c = config::Config::load(path.clone());
        let id = c.id();
        let salt = c.salt();
        let uuid = c.uuid();
        ok("id/salt/uuid generated", !id.is_empty() && !salt.is_empty() && !uuid.is_empty(),
           &format!("id={} salt={} chars, uuid={} bytes", id, salt.len(), uuid.len()));
        match c.store() {
            Ok(()) => {
                let again = config::Config::load(path.clone());
                let stable = again.get("id").map(|s| s.to_owned()) == Some(id.clone());
                ok("store + reload", stable, &format!("id round trip {}", stable));
            }
            Err(e) => ok("store + reload", false, &format!("store failed: {}", e)),
        }
        let _ = std::fs::remove_file(&path);
    }

    head("http (real TCP to the build host)");
    {
        match http::Url::parse("http://192.168.0.1:8099/") {
            Ok(u) => {
                ok("Url::parse", true, &format!("host={} port={}", u.host, u.port));
                match std::net::TcpStream::connect(("192.168.0.1", 8099)) {
                    Ok(mut s) => {
                        use std::io::{Read, Write};
                        let _ = s.write_all(b"GET /minimal.c HTTP/1.0\r\nHost: build\r\n\r\n");
                        let mut buf = Vec::new();
                        let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(20)));
                        let n = s.read_to_end(&mut buf);
                        let head200 = buf.starts_with(b"HTTP/1.0 200") || buf.starts_with(b"HTTP/1.1 200");
                        ok("TcpStream GET", head200,
                           &format!("read {:?} bytes, first line: {}", n.map(|_| buf.len()),
                                    String::from_utf8_lossy(&buf[..buf.len().min(24)]).trim().to_owned()));
                    }
                    Err(e) => ok("TcpStream GET", false, &format!("connect failed: {}", e)),
                }
            }
            Err(e) => ok("Url::parse", false, &e),
        }
    }

    head("protobuf");
    {
        use protobuf::Message as _;
        let mut msg = rustdesk_ppc_agent::rendezvous_proto::RendezvousMessage::new();
        let mut ph = rustdesk_ppc_agent::rendezvous_proto::RegisterPeer::new();
        ph.id = "IRIS-0001".to_owned();
        msg.set_register_peer(ph);
        match msg.write_to_bytes() {
            Ok(bytes) => {
                let back = rustdesk_ppc_agent::rendezvous_proto::RendezvousMessage::parse_from_bytes(&bytes);
                let good = back.map(|m| m.has_register_peer()).unwrap_or(false);
                ok("encode + decode", good, &format!("{} bytes on the wire", bytes.len()));
            }
            Err(e) => ok("encode + decode", false, &format!("{}", e)),
        }
    }

    println!("\nself-test finished");
}
