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

/// What the configuration file is called, per port.
///
/// Solaris ships as `rdeskvint` in a package that is new, so its file is named
/// for the product. The other ports keep the old name deliberately: installs of
/// them exist, and this file holds the machine's ID, its uuid and its signing
/// key -- see `LEGACY_NAME` for why losing track of it is worse than an
/// inconsistent filename.
#[cfg(target_os = "solaris")]
const CONFIG_NAME: &str = ".rdeskvint.conf";
#[cfg(not(target_os = "solaris"))]
const CONFIG_NAME: &str = ".rustdesk-ppc-agent.conf";

/// What this file was called before the rename, and still is on the other
/// ports.
///
/// It is read when the current name is absent, because the alternative is
/// silently generating a fresh identity: a new ID, a new keypair, and -- worst
/// of the three -- a new uuid. hbbs pins the first uuid it sees for an id and
/// answers UUID_MISMATCH to every later one for ever, so a machine that
/// "upgraded" into a new config file does not re-register under a new name. It
/// stops being able to register at all.
const LEGACY_NAME: &str = ".rustdesk-ppc-agent.conf";

/// Settings that have been renamed, old spelling first. Applied on load, so a
/// file written by an older build keeps working and is rewritten under the new
/// names the next time anything is set.
const RENAMED: &[(&str, &str)] = &[("rendezvous_server", "id_server"), ("server_key", "key")];

/// A configuration shared by every session on the machine.
///
/// A per-user file is the right shape when one person owns a machine. It is the
/// wrong shape here, because this agent runs *inside* whoever's session is on
/// the console -- so the identity would change with the account that logged in.
/// That is not a cosmetic problem: a second account means a second ID, a second
/// row in the console's device list, and a second uuid, which hbbs will refuse
/// the moment it has already pinned one to that ID.
///
/// So when this file exists it wins, for everybody. It has to be readable by
/// every account that logs in, which is why the tool that creates it says out
/// loud who that is: it holds the connection password and the agent's private
/// key.
#[cfg(target_os = "solaris")]
const SHARED_PATH: Option<&str> = Some("/etc/opt/rdeskvint/agent.conf");
#[cfg(not(target_os = "solaris"))]
const SHARED_PATH: Option<&str> = None;

#[derive(Default)]
pub struct Config {
    path: PathBuf,
    kv: BTreeMap<String, String>,
    /// Set when the values came from `LEGACY_NAME` rather than from `path`, so
    /// the agent can say so once rather than leaving somebody to wonder which
    /// of two files on the disk is the live one.
    migrated_from: Option<PathBuf>,
}

impl Config {
    pub fn default_path() -> PathBuf {
        // The shared file first, when there is one. Only its existence decides:
        // a machine either has one identity for every session or it does not,
        // and making that depend on who is logged in is the bug this avoids.
        if let Some(shared) = SHARED_PATH {
            let shared = Path::new(shared);
            if shared.exists() {
                return shared.to_path_buf();
            }
        }
        let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
        Path::new(&home).join(CONFIG_NAME)
    }

    /// Where a machine-wide configuration would live on this platform.
    pub fn shared_path() -> Option<PathBuf> {
        SHARED_PATH.map(PathBuf::from)
    }

    /// The pre-rename name beside `path`, when there is a distinct one.
    ///
    /// Derived from `path` rather than from `$HOME` so that `--config` keeps
    /// working: point it at a directory and the same migration applies there.
    fn legacy_path(path: &Path) -> Option<PathBuf> {
        if CONFIG_NAME == LEGACY_NAME {
            return None;
        }
        match path.file_name() {
            Some(n) if n == CONFIG_NAME => Some(path.with_file_name(LEGACY_NAME)),
            _ => None,
        }
    }

    pub fn load(path: PathBuf) -> Self {
        let mut kv = BTreeMap::new();
        let mut migrated_from = None;

        // Read the current name; fall back to the old one if it is not there
        // yet. The old file is left where it is rather than moved: the next
        // `store()` writes the new name, and deleting somebody's only copy of a
        // signing key to tidy up a filename is not a trade worth making.
        let mut source = path.clone();
        if !source.exists() {
            if let Some(legacy) = Self::legacy_path(&path) {
                if legacy.exists() {
                    migrated_from = Some(legacy.clone());
                    source = legacy;
                }
            }
        }

        if let Ok(s) = std::fs::read_to_string(&source) {
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

        // These four settings are the four fields a stock RustDesk client shows
        // -- ID Server, Relay Server, API Server, Key -- and they are named for
        // them, because somebody configuring both should not have to hold a
        // translation table. Two of them used to be called something else, from
        // back when the names came from the protocol rather than from the UI.
        // Read the old spelling, write the new one.
        for (was, now) in RENAMED {
            if let Some(v) = kv.remove(*was) {
                kv.entry((*now).to_owned()).or_insert(v);
            }
        }

        Self { path, kv, migrated_from }
    }

    /// The file these values came out of, and where a change will be written.
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The old file these values were read out of, if they were.
    pub fn migrated_from(&self) -> Option<&Path> {
        self.migrated_from.as_deref()
    }

    pub fn store(&self) -> io::Result<()> {
        // Grouped, because the two halves of this file are not alike and the
        // difference is invisible when they are sorted together: half of it is
        // this machine's identity, which is generated once and must survive,
        // and half is settings somebody chose. Comments are skipped by `load`,
        // so this costs nothing to read back.
        const IDENTITY: &[&str] = &["id", "uuid", "salt", "public_key", "secret_key"];
        // Ordered as a client presents them, not alphabetically.
        const SETTINGS: &[&str] = &[
            "password",
            "id_server",
            "relay_server",
            "api_server",
            "key",
            "ca_bundle",
            "scale",
        ];

        let mut out = String::from(
            "# R-DeskVint agent -- written by the agent; use its flags, not an editor\n",
        );
        let mut emit = |out: &mut String, k: &str| {
            if let Some(v) = self.kv.get(k) {
                out.push_str(k);
                out.push_str(" = ");
                out.push_str(v);
                out.push('\n');
            }
        };

        out.push_str(
            "\n# This machine's identity. Generated once, and not yours to pick.\n\
             # Losing it makes this a different machine to every peer and to the\n\
             # server, which pins the first uuid it sees for an id for ever.\n",
        );
        for k in IDENTITY {
            emit(&mut out, k);
        }

        out.push_str("\n# Settings. Empty means the default; see --help.\n");
        for k in SETTINGS {
            emit(&mut out, k);
        }

        // Anything a newer or older build wrote that this one does not know
        // about. Kept rather than dropped: a round trip through an old binary
        // should not silently delete a setting a new one added.
        let known: Vec<&str> = IDENTITY.iter().chain(SETTINGS.iter()).copied().collect();
        let unknown: Vec<&String> = self.kv.keys().filter(|k| !known.contains(&k.as_str())).collect();
        if !unknown.is_empty() {
            out.push_str("\n# Not written by this version, and kept as found.\n");
            for k in unknown {
                out.push_str(k);
                out.push_str(" = ");
                out.push_str(self.kv.get(k).map(|s| s.as_str()).unwrap_or(""));
                out.push('\n');
            }
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
        self.get("id_server").unwrap_or_default().to_owned()
    }

    pub fn set_rendezvous_server(&mut self, s: &str) -> io::Result<()> {
        self.set("id_server", s);
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
        self.get("key").unwrap_or_default().to_owned()
    }

    pub fn set_server_key(&mut self, k: &str) -> io::Result<()> {
        self.set("key", k);
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

    /// The console this machine reports in to, so it appears in a device list.
    ///
    /// Empty by default, and entirely separate from `rendezvous_server`: that
    /// one makes the machine *reachable*, this one makes it *visible*. A
    /// deployment can sensibly have either, both or neither -- the console
    /// builds its list from the HTTP API and not from hbbs registration, so
    /// registering without reporting in is a machine that connects by id and
    /// never appears anywhere. See `api` for the contract.
    pub fn api_server(&self) -> String {
        self.get("api_server").unwrap_or_default().to_owned()
    }

    pub fn set_api_server(&mut self, s: &str) -> io::Result<()> {
        self.set("api_server", s);
        self.store()
    }

    /// The downscale the video starts at, or 0 for the platform's own default.
    ///
    /// A resolution dial rather than a quality one: 1 serves the framebuffer at
    /// full size, 2 at half in each direction, and so on in powers of two. A
    /// peer that asks for a particular image quality still overrides it for
    /// that session.
    pub fn scale(&self) -> usize {
        self.get("scale").and_then(|s| s.parse().ok()).unwrap_or(0)
    }

    pub fn set_scale(&mut self, s: usize) -> io::Result<()> {
        if s == 0 {
            self.set("scale", "");
        } else {
            self.set("scale", &s.to_string());
        }
        self.store()
    }

    /// A CA bundle to verify an https console against.
    ///
    /// Empty means the usual places are searched -- see `http::CaBundle`. It is
    /// a setting rather than a constant because a self-hosted console is very
    /// often behind a private CA, and because this platform's own trust store
    /// is too old to verify most of the public internet.
    pub fn ca_bundle(&self) -> String {
        self.get("ca_bundle").unwrap_or_default().to_owned()
    }

    pub fn set_ca_bundle(&mut self, p: &str) -> io::Result<()> {
        self.set("ca_bundle", p);
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

/// Standard base64, with padding.
///
/// Lives beside `hex_encode` because it is the same job -- binary identity
/// bytes rendered as text -- for the two places that need the other alphabet:
/// the public key `--show-key` prints for a peer to pin, and the `uuid` the
/// console API expects. No `=` is ever omitted: the console compares the string
/// it was first sent against the one it is sent later, so the encoding has to
/// be stable rather than merely decodable.
pub fn base64_encode(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for c in b.chunks(3) {
        let x = [c[0], *c.get(1).unwrap_or(&0), *c.get(2).unwrap_or(&0)];
        let n = ((x[0] as u32) << 16) | ((x[1] as u32) << 8) | x[2] as u32;
        out.push(T[(n >> 18 & 63) as usize] as char);
        out.push(T[(n >> 12 & 63) as usize] as char);
        out.push(if c.len() > 1 { T[(n >> 6 & 63) as usize] as char } else { '=' });
        out.push(if c.len() > 2 { T[(n & 63) as usize] as char } else { '=' });
    }
    out
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

    /// RFC 4648 vectors, including both padding lengths. The console pins the
    /// first uuid string it is sent and ignores any heartbeat that does not
    /// match it, so an encoder that dropped padding would register the machine
    /// once and then go quiet.
    #[test]
    fn base64_matches_the_standard_alphabet_and_keeps_its_padding() {
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foob"), "Zm9vYg==");
        assert_eq!(base64_encode(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
        // The two characters that separate this from the URL-safe alphabet.
        assert_eq!(base64_encode(&[0xff, 0xef]), "/+8=");
    }

    /// The same bytes must encode the same way every run, on either endianness.
    #[test]
    fn base64_is_stable_for_a_uuid() {
        let uuid: Vec<u8> = (0..16u8).collect();
        assert_eq!(base64_encode(&uuid), base64_encode(&uuid));
        assert_eq!(base64_encode(&uuid), "AAECAwQFBgcICQoLDA0ODw==");
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
