//! The RustDesk secure channel and agent-side handshake.
//!
//! Reimplements, on blocking I/O:
//!   * `libs/hbb_common/src/tcp.rs`      — the secretbox channel (send/next/get_nonce)
//!   * `src/server.rs:90-135`            — the agent side of the key exchange
//!   * `src/server/connection.rs:596-600`— the password check
//!
//! ## Wire spec (agent side)
//!
//! 1. The agent holds a long-term **Ed25519** signing keypair. Peers learn its
//!    public half out of band (via the ID server, or pasted for direct-IP use).
//! 2. Per connection, the agent generates an ephemeral **X25519** box keypair
//!    and sends, *unencrypted*:
//!
//! ```text
//! Message{ signed_id: SignedId{ id: sign(IdPk{ id, pk: ephemeral_pk }) } }
//! ```
//!
//!    Note `sign::sign` — the **combined** form (signature ‖ message), not
//!    `sign_detached` — and that the signed message is a serialised `IdPk`
//!    protobuf carrying the **raw** 32-byte key. 1.1.8 signed the plaintext
//!    string `"<id>\0<base64(pk)>"` here; see `signed_id` for why that is not
//!    what we send.
//! 3. The peer replies with `Message{ public_key: PublicKey{ asymmetric_value,
//!    symmetric_value } }`, where `asymmetric_value` is its own 32-byte X25519
//!    public key and `symmetric_value` is the secretbox key sealed with
//!    `box_::seal(key, nonce = [0u8; 24], agent_ephemeral_pk, peer_sk)`.
//!    **The box nonce is all zeros** — safe only because both keypairs are
//!    ephemeral per connection.
//! 4. The agent opens it and from then on every frame body is
//!    `secretbox::seal/open` (XSalsa20-Poly1305) with a **counter nonce**:
//!    separate send and receive counters, both starting at 0 and
//!    **pre-incremented**, so the first frame each way uses nonce 1. The nonce
//!    is the counter as little-endian u64 in the low 8 of 24 bytes, rest zero.
//!    `u64::to_le_bytes` is explicit, so this is correct on big-endian PowerPC.
//!
//! Then the login exchange: agent sends `Hash{ salt, challenge }`, peer replies
//! `LoginRequest{ password, .. }`, and the agent checks
//! `sha256(sha256(permanent_password ‖ salt) ‖ challenge)`.

use protobuf::Message as _;
use sodiumoxide::crypto::{box_, secretbox, sign};

use crate::message_proto::IdPk;

/// Sealed frames plus the two nonce counters. Mirrors the `(Key, u64, u64)`
/// tuple in hbb_common's `FramedStream`.
pub struct SecureChannel {
    key: secretbox::Key,
    send_seq: u64,
    recv_seq: u64,
}

impl SecureChannel {
    pub fn new(key: secretbox::Key) -> Self {
        Self { key, send_seq: 0, recv_seq: 0 }
    }

    /// Counter -> 24-byte nonce, low 8 bytes little-endian.
    fn nonce(seq: u64) -> secretbox::Nonce {
        let mut n = [0u8; secretbox::NONCEBYTES];
        n[..8].copy_from_slice(&seq.to_le_bytes());
        secretbox::Nonce(n)
    }

    /// Pre-increments, so the first sealed frame uses nonce 1 (upstream does
    /// `key.1 += 1` before sealing).
    pub fn seal(&mut self, plain: &[u8]) -> Vec<u8> {
        self.send_seq += 1;
        secretbox::seal(plain, &Self::nonce(self.send_seq), &self.key)
    }

    pub fn open(&mut self, cipher: &[u8]) -> Result<Vec<u8>, ()> {
        self.recv_seq += 1;
        secretbox::open(cipher, &Self::nonce(self.recv_seq), &self.key)
    }
}

/// The agent's per-connection ephemeral X25519 keypair, held between sending
/// `SignedId` and opening the peer's sealed symmetric key.
pub struct Handshake {
    our_pk_b: box_::PublicKey,
    our_sk_b: box_::SecretKey,
}

impl Handshake {
    pub fn new() -> Self {
        let (our_pk_b, our_sk_b) = box_::gen_keypair();
        Self { our_pk_b, our_sk_b }
    }

    /// The payload for `SignedId.id`: `sign(IdPk{ id, pk }, sk)`, combined form
    /// — signature and message together.
    ///
    /// **The message is a protobuf, not a string.** 1.1.8 signed
    /// `"<id>\0<base64(ephemeral_pk)>"` and this agent did too, which is a
    /// format no modern client can read: `decode_id_pk` (the client's
    /// `src/common.rs`) verifies the signature — which succeeded, our key being
    /// fine — and then runs `IdPk::parse_from_bytes` over it, which fails. The
    /// client answers a failed parse with an **empty `PublicKey`**, the same
    /// thing it sends when it has no key for us, so every session through the
    /// rendezvous server silently fell back to plaintext while the client
    /// logged "pk mismatch". The pk was never the problem; the encoding was.
    ///
    /// `pk` goes in as raw bytes — the old format base64'd it, this one does
    /// not. Mirrors `src/server.rs:210-225` in current master.
    pub fn signed_id(&self, id: &str, sk: &sign::SecretKey) -> Vec<u8> {
        let mut idpk = IdPk::new();
        idpk.id = id.to_owned();
        idpk.pk = self.our_pk_b.0.to_vec();
        // `write_to_bytes` fails only on a message with unset required fields,
        // which proto3 does not have. Upstream does the same with `unwrap_or_default`.
        sign::sign(&idpk.write_to_bytes().unwrap_or_default(), sk)
    }

    /// Open the peer's sealed secretbox key. `asymmetric_value` is its X25519
    /// public key, `symmetric_value` the sealed key (zero nonce).
    pub fn open_symmetric_key(
        &self,
        asymmetric_value: &[u8],
        symmetric_value: &[u8],
    ) -> Result<secretbox::Key, &'static str> {
        if asymmetric_value.len() != box_::PUBLICKEYBYTES {
            return Err("handshake: peer public key has the wrong length");
        }
        let mut pk_ = [0u8; box_::PUBLICKEYBYTES];
        pk_.copy_from_slice(asymmetric_value);
        let their_pk_b = box_::PublicKey(pk_);
        let nonce = box_::Nonce([0u8; box_::NONCEBYTES]);
        let opened = box_::open(symmetric_value, &nonce, &their_pk_b, &self.our_sk_b)
            .map_err(|_| "handshake: box decryption failure")?;
        if opened.len() != secretbox::KEYBYTES {
            return Err("handshake: invalid secret key length from peer");
        }
        let mut key = [0u8; secretbox::KEYBYTES];
        key.copy_from_slice(&opened);
        Ok(secretbox::Key(key))
    }
}

/// `sha256(sha256(password ‖ salt) ‖ challenge)` — connection.rs:596-600.
pub fn expected_login_hash(password: &str, salt: &[u8], challenge: &[u8]) -> Vec<u8> {
    use sha2::{Digest, Sha256};
    let mut h1 = Sha256::new();
    h1.update(password.as_bytes());
    h1.update(salt);
    let mut h2 = Sha256::new();
    h2.update(&h1.finalize()[..]);
    h2.update(challenge);
    h2.finalize().to_vec()
}

/// Constant-time comparison, so a wrong password cannot be recovered a byte at
/// a time by timing. (Upstream uses `!=` on slices here; we can do better for
/// free.)
pub fn verify_login_hash(expected: &[u8], got: &[u8]) -> bool {
    if expected.len() != got.len() {
        return false;
    }
    let mut diff = 0u8;
    for (a, b) in expected.iter().zip(got.iter()) {
        diff |= a ^ b;
    }
    diff == 0
}

// A base64 encoder lived here, for the `<id>\0<base64(pk)>` payload. `IdPk`
// carries the key as raw bytes, so the agent no longer needs base64 anywhere:
// the only other place a key is written out is `config.rs`, which uses hex.

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nonce_is_little_endian_regardless_of_host_byte_order() {
        // The G5 is big-endian; `to_le_bytes` must still produce LE.
        let n = SecureChannel::nonce(1);
        assert_eq!(&n.0[..8], &[1, 0, 0, 0, 0, 0, 0, 0]);
        let n = SecureChannel::nonce(0x0102_0304_0506_0708);
        assert_eq!(&n.0[..8], &[8, 7, 6, 5, 4, 3, 2, 1]);
        assert!(n.0[8..].iter().all(|&b| b == 0));
    }

    #[test]
    fn channel_roundtrips_and_counters_advance_together() {
        let key = secretbox::gen_key();
        let mut a = SecureChannel::new(key.clone());
        let mut b = SecureChannel::new(key);
        for i in 0..4u8 {
            let msg = vec![i; 16];
            let sealed = a.seal(&msg);
            assert_eq!(b.open(&sealed).unwrap(), msg, "frame {}", i);
        }
        assert_eq!(a.send_seq, 4);
        assert_eq!(b.recv_seq, 4);
    }

    #[test]
    fn first_frame_uses_nonce_one_not_zero() {
        let key = secretbox::gen_key();
        let mut ch = SecureChannel::new(key.clone());
        let sealed = ch.seal(b"hello");
        // Decrypting with nonce 0 must fail; with nonce 1 must succeed.
        assert!(secretbox::open(&sealed, &SecureChannel::nonce(0), &key).is_err());
        assert_eq!(
            secretbox::open(&sealed, &SecureChannel::nonce(1), &key).unwrap(),
            b"hello"
        );
    }

    #[test]
    fn out_of_order_frame_fails_to_open() {
        let key = secretbox::gen_key();
        let mut a = SecureChannel::new(key.clone());
        let mut b = SecureChannel::new(key);
        let f1 = a.seal(b"one");
        let f2 = a.seal(b"two");
        // Receiver expects f1 first; handing it f2 must not authenticate.
        assert!(b.open(&f2).is_err());
        let _ = f1;
    }

    #[test]
    fn handshake_key_exchange_roundtrip() {
        // Stand in for the peer: seal a fresh secretbox key to the agent's
        // ephemeral public key with a zero nonce, exactly as client.rs does.
        let agent = Handshake::new();
        let (peer_pk, peer_sk) = box_::gen_keypair();
        let sym = secretbox::gen_key();
        let sealed = box_::seal(
            &sym.0,
            &box_::Nonce([0u8; box_::NONCEBYTES]),
            &agent.our_pk_b,
            &peer_sk,
        );
        let got = agent.open_symmetric_key(&peer_pk.0, &sealed).unwrap();
        assert_eq!(got.0, sym.0);
    }

    #[test]
    fn handshake_rejects_malformed_peer_key() {
        let agent = Handshake::new();
        assert!(agent.open_symmetric_key(&[0u8; 31], &[0u8; 48]).is_err());
        // Right length, wrong key: box open must fail rather than yield garbage.
        assert!(agent.open_symmetric_key(&[7u8; 32], &[0u8; 48]).is_err());
    }

    /// The signed payload, pinned against a vector assembled from the protobuf
    /// encoding rules rather than from our own codegen.
    ///
    /// The test this replaced encoded and decoded with this module's own code,
    /// so it agreed with itself perfectly — and would have agreed just as
    /// perfectly with an invented format, which is what it was in fact
    /// certifying for months. Same reasoning, and the same class of bug, as
    /// `rendezvous::mangle_matches_the_wire_format`.
    #[test]
    fn signed_id_matches_the_wire_format() {
        // Both keys from seeds, so the output is fixed rather than fresh.
        let (verify_pk, sk) = sign::keypair_from_seed(&sign::Seed([0x42u8; 32]));
        let (our_pk_b, our_sk_b) = box_::keypair_from_seed(&box_::Seed([0x11u8; 32]));
        let agent = Handshake { our_pk_b, our_sk_b };

        let signed = agent.signed_id("4iv3930za", &sk);
        let msg = sign::verify(&signed, &verify_pk).expect("signature must verify");

        // Hand-assembled from the wire spec, not from `IdPk::write_to_bytes`:
        //   field 1 `id`, wire type 2 -> tag (1<<3)|2 = 0x0a, length, UTF-8
        //   field 2 `pk`, wire type 2 -> tag (2<<3)|2 = 0x12, length, RAW key
        // The key is 32 raw bytes; base64 here is what the old format did and
        // is what no client could read.
        let mut want = vec![0x0a, 9];
        want.extend_from_slice(b"4iv3930za");
        want.extend_from_slice(&[0x12, 32]);
        want.extend_from_slice(&agent.our_pk_b.0);
        assert_eq!(msg, want);

        // 64-byte signature + 11 + 34 = 109, which is also the size of the
        // signed `IdPk` a real hbbs hands a caller for a 9-character id --
        // the one length here that was measured off another implementation.
        assert_eq!(signed.len(), 109);
    }

    /// Decoded the way the *client* decodes it: verify, then parse `IdPk` —
    /// `decode_id_pk` in upstream's `src/common.rs`, which is the code that
    /// silently gave up on us. Splitting on a separator we chose is what the
    /// old test did.
    #[test]
    fn signed_id_decodes_the_way_a_modern_client_decodes_it() {
        let (pk, sk) = sign::gen_keypair();
        let agent = Handshake::new();
        let signed = agent.signed_id("123456789", &sk);

        let opened = sign::verify(&signed, &pk).expect("signature must verify");
        let idpk = IdPk::parse_from_bytes(&opened).expect("client parses SignedId.id as IdPk");
        assert_eq!(idpk.id, "123456789");
        // The client's `get_pk` rejects anything that is not exactly 32 bytes,
        // and answers with an empty PublicKey when it does.
        assert_eq!(idpk.pk.len(), box_::PUBLICKEYBYTES);
        assert_eq!(idpk.pk, agent.our_pk_b.0.to_vec());
    }

    #[test]
    fn login_hash_is_stable_and_verifies_in_constant_time() {
        let salt = b"somesalt";
        let challenge = b"somechallenge";
        let h = expected_login_hash("hunter2", salt, challenge);
        assert_eq!(h.len(), 32);
        assert!(verify_login_hash(&h, &expected_login_hash("hunter2", salt, challenge)));
        assert!(!verify_login_hash(&h, &expected_login_hash("hunter3", salt, challenge)));
        assert!(!verify_login_hash(&h, &h[..31]));
    }
}
