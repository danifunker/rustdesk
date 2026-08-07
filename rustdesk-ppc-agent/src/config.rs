//! Persistent agent identity and settings.
//!
//! Mirrors the pieces of `libs/hbb_common/src/config.rs` the agent actually
//! needs: an ID, a permanent password, a persisted salt, and the long-term
//! Ed25519 signing keypair whose public half peers use to authenticate us.
//!
//! Upstream stores this as TOML via `confy`. We write a trivial `key = value`
//! file instead — it avoids serde/toml entirely, and the whole schema is six
//! scalars.

use std::collections::BTreeMap;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use sodiumoxide::crypto::sign;

/// Upstream's alphabet for generated IDs, salts and challenges
/// (`config.rs`'s `CHARS`) — digits and lowercase, no ambiguous glyphs removed,
/// so anything we generate is accepted by a stock peer.
const CHARS: &[u8] = b"1234567890abcdefghijklmnopqrstuvwxyz";

#[derive(Default)]
pub struct Config {
    path: PathBuf,
    kv: BTreeMap<String, String>,
}

impl Config {
    pub fn default_path() -> PathBuf {
        let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
        Path::new(&home).join(".rustdesk-ppc-agent.conf")
    }

    pub fn load(path: PathBuf) -> Self {
        let mut kv = BTreeMap::new();
        if let Ok(s) = std::fs::read_to_string(&path) {
            for line in s.lines() {
                let line = line.trim();
                if line.is_empty() || line.starts_with('#') {
                    continue;
                }
                if let Some(eq) = line.find('=') {
                    kv.insert(line[..eq].trim().to_owned(), line[eq + 1..].trim().to_owned());
                }
            }
        }
        Self { path, kv }
    }

    pub fn store(&self) -> io::Result<()> {
        let mut out = String::from("# rustdesk-ppc-agent\n");
        for (k, v) in &self.kv {
            out.push_str(k);
            out.push_str(" = ");
            out.push_str(v);
            out.push('\n');
        }
        let tmp = self.path.with_extension("tmp");
        {
            let mut f = std::fs::File::create(&tmp)?;
            f.write_all(out.as_bytes())?;
            f.sync_all()?;
        }
        std::fs::rename(&tmp, &self.path)?;
        // The signing secret key lives in here.
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&self.path, std::fs::Permissions::from_mode(0o600));
        }
        Ok(())
    }

    pub fn get(&self, k: &str) -> Option<&str> {
        self.kv.get(k).map(|s| s.as_str())
    }

    pub fn set(&mut self, k: &str, v: &str) {
        self.kv.insert(k.to_owned(), v.to_owned());
    }

    /// `n` characters from upstream's alphabet. Used for the ID, the salt, and
    /// the per-connection challenge.
    pub fn random_string(n: usize) -> String {
        let mut buf = vec![0u8; n];
        sodiumoxide::randombytes::randombytes_into(&mut buf);
        buf.iter().map(|b| CHARS[*b as usize % CHARS.len()] as char).collect()
    }

    /// Generated once and persisted. Upstream derives a numeric ID from the
    /// machine UID; a stable random one is equivalent for direct-IP use and
    /// avoids depending on Leopard's hardware identifiers.
    pub fn id(&mut self) -> String {
        if self.get("id").map_or(true, |s| s.is_empty()) {
            let id = Self::random_string(9);
            self.set("id", &id);
            let _ = self.store();
        }
        self.get("id").unwrap_or_default().to_owned()
    }

    pub fn salt(&mut self) -> String {
        if self.get("salt").map_or(true, |s| s.is_empty()) {
            let salt = Self::random_string(6);
            self.set("salt", &salt);
            let _ = self.store();
        }
        self.get("salt").unwrap_or_default().to_owned()
    }

    /// The machine identifier a rendezvous server pins our id to.
    ///
    /// **It must never change.** hbbs stores the first uuid it sees for an id
    /// and answers `UUID_MISMATCH` to anything else for ever after, so a
    /// regenerated uuid does not re-register, it locks us out. Upstream derives
    /// this from hardware; a persisted random value is equivalent and avoids
    /// depending on Leopard's identifiers, exactly as `id` does.
    pub fn uuid(&mut self) -> Vec<u8> {
        if let Some(hex) = self.get("uuid") {
            if let Some(b) = hex_decode(hex) {
                if !b.is_empty() {
                    return b;
                }
            }
        }
        let mut b = vec![0u8; 16];
        sodiumoxide::randombytes::randombytes_into(&mut b);
        self.set("uuid", &hex_encode(&b));
        let _ = self.store();
        b
    }

    /// The rendezvous server to register with, if one is configured.
    pub fn rendezvous_server(&self) -> String {
        self.get("rendezvous_server").unwrap_or_default().to_owned()
    }

    pub fn set_rendezvous_server(&mut self, s: &str) -> io::Result<()> {
        self.set("rendezvous_server", s);
        self.store()
    }

    /// The self-hosted server's key — the same base64 string a RustDesk client
    /// puts in its "Key" field. Sent as `RequestRelay.licence_key` when joining
    /// a relay, and needed only against an hbbr started with `-k`; an unkeyed
    /// relay ignores it. Empty by default, which is the common case.
    ///
    /// Not to be confused with `public_key`/`secret_key`, which are this
    /// agent's own identity. This one belongs to the server.
    pub fn server_key(&self) -> String {
        self.get("server_key").unwrap_or_default().to_owned()
    }

    pub fn set_server_key(&mut self, k: &str) -> io::Result<()> {
        self.set("server_key", k);
        self.store()
    }

    /// A relay to use instead of the one the rendezvous server advertises.
    ///
    /// Empty by default, which is right: hbbs advertises a relay to peers that
    /// do not set their own, and upstream's `get_relay_server` prefers the
    /// local option only when it is present. Worth having because the server
    /// can advertise something a remote caller cannot reach -- a LAN address,
    /// which is one of the three faults in BACKLOG.md item 12 -- and this is
    /// the escape hatch for it that does not require touching the server.
    pub fn relay_server(&self) -> String {
        self.get("relay_server").unwrap_or_default().to_owned()
    }

    pub fn set_relay_server(&mut self, s: &str) -> io::Result<()> {
        self.set("relay_server", s);
        self.store()
    }

    pub fn password(&self) -> String {
        self.get("password").unwrap_or_default().to_owned()
    }

    pub fn set_password(&mut self, p: &str) -> io::Result<()> {
        self.set("password", p);
        self.store()
    }

    /// The long-term Ed25519 identity. Peers learn the public half out of band
    /// and use it to verify the `SignedId` we send at handshake.
    pub fn key_pair(&mut self) -> (sign::PublicKey, sign::SecretKey) {
        if let (Some(pk), Some(sk)) = (self.get("public_key"), self.get("secret_key")) {
            if let (Some(pk), Some(sk)) = (hex_decode(pk), hex_decode(sk)) {
                if pk.len() == sign::PUBLICKEYBYTES && sk.len() == sign::SECRETKEYBYTES {
                    let mut pkb = [0u8; sign::PUBLICKEYBYTES];
                    let mut skb = [0u8; sign::SECRETKEYBYTES];
                    pkb.copy_from_slice(&pk);
                    skb.copy_from_slice(&sk);
                    return (sign::PublicKey(pkb), sign::SecretKey(skb));
                }
            }
        }
        let (pk, sk) = sign::gen_keypair();
        self.set("public_key", &hex_encode(&pk.0));
        self.set("secret_key", &hex_encode(&sk.0));
        let _ = self.store();
        (pk, sk)
    }
}

fn hex_encode(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push_str(&format!("{:02x}", x));
    }
    s
}

fn hex_decode(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).ok())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp_cfg() -> Config {
        let mut p = std::env::temp_dir();
        p.push(format!("rdppc-test-{}.conf", Config::random_string(8)));
        Config::load(p)
    }

    #[test]
    fn hex_roundtrips() {
        let b: Vec<u8> = (0..=255u8).collect();
        assert_eq!(hex_decode(&hex_encode(&b)).unwrap(), b);
        assert!(hex_decode("abc").is_none());
        assert!(hex_decode("zz").is_none());
    }

    #[test]
    fn random_string_uses_the_upstream_alphabet() {
        let s = Config::random_string(256);
        assert_eq!(s.len(), 256);
        assert!(s.bytes().all(|b| CHARS.contains(&b)));
    }

    #[test]
    fn identity_is_stable_across_reloads() {
        let mut c = tmp_cfg();
        let path = c.path.clone();
        let (id, salt) = (c.id(), c.salt());
        let (pk, sk) = c.key_pair();

        let mut c2 = Config::load(path.clone());
        assert_eq!(c2.id(), id, "id must persist");
        assert_eq!(c2.salt(), salt, "salt must persist");
        let (pk2, sk2) = c2.key_pair();
        assert_eq!(pk2.0, pk.0, "public key must persist");
        assert_eq!(sk2.0[..], sk.0[..], "secret key must persist");
        let _ = std::fs::remove_file(path);
    }

    /// The whole point of the field: a uuid that moves locks the agent out of
    /// its own id, permanently, with `UUID_MISMATCH`.
    #[test]
    fn uuid_is_stable_across_reloads() {
        let mut c = tmp_cfg();
        let path = c.path.clone();
        let u = c.uuid();
        assert_eq!(u.len(), 16);
        assert_ne!(u, vec![0u8; 16], "must not be all zeroes");
        assert_eq!(c.uuid(), u, "stable within one instance");
        assert_eq!(Config::load(path.clone()).uuid(), u, "stable across a reload");
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn rendezvous_server_roundtrips() {
        let mut c = tmp_cfg();
        let path = c.path.clone();
        assert_eq!(c.rendezvous_server(), "");
        c.set_rendezvous_server("rustdesk.example.org").unwrap();
        assert_eq!(
            Config::load(path.clone()).rendezvous_server(),
            "rustdesk.example.org"
        );
        let _ = std::fs::remove_file(path);
    }

    /// Empty by default, because that is what an unkeyed hbbr wants and a key
    /// invented here would be worse than none: hbbr drops a mismatch silently.
    #[test]
    fn server_key_roundtrips_and_defaults_to_empty() {
        let mut c = tmp_cfg();
        let path = c.path.clone();
        assert_eq!(c.server_key(), "");
        // Base64 with '+' and '=' in it, since that is the shape of a real one
        // and the config format has to carry it unmangled.
        c.set_server_key("BSJl5A+1EXS3omQkgTiXGvdKm9HFzE50bYrf5Je4tdU=").unwrap();
        assert_eq!(
            Config::load(path.clone()).server_key(),
            "BSJl5A+1EXS3omQkgTiXGvdKm9HFzE50bYrf5Je4tdU="
        );
        // `--key ''` clears it rather than being ignored.
        c.set_server_key("").unwrap();
        assert_eq!(Config::load(path.clone()).server_key(), "");
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn password_roundtrips_and_survives_reload() {
        let mut c = tmp_cfg();
        let path = c.path.clone();
        assert_eq!(c.password(), "");
        c.set_password("hunter2 with spaces").unwrap();
        assert_eq!(Config::load(path.clone()).password(), "hunter2 with spaces");
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn generated_keypair_actually_signs() {
        let mut c = tmp_cfg();
        let path = c.path.clone();
        let (pk, sk) = c.key_pair();
        let signed = sign::sign(b"payload", &sk);
        assert_eq!(sign::verify(&signed, &pk).unwrap(), b"payload");
        let _ = std::fs::remove_file(path);
    }
}
