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
//! Message{ signed_id: SignedId{ id: sign("<id>\0<base64(ephemeral_pk)>") } }
//! ```
//!
//!    Note `sign::sign` — the **combined** form (signature ‖ message), not
//!    `sign_detached`.
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

use sodiumoxide::crypto::{box_, secretbox, sign};

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

    /// The payload for `SignedId.id`: `sign("<id>\0<base64(ephemeral_pk)>", sk)`.
    /// Combined form — signature and message together.
    pub fn signed_id(&self, id: &str, sk: &sign::SecretKey) -> Vec<u8> {
        let msg = format!("{}\0{}", id, base64_encode(&self.our_pk_b.0));
        sign::sign(msg.as_bytes(), sk)
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

/// Standard base64, matching the `base64` crate's default alphabet as used by
/// `src/server.rs`. Inlined to keep the dependency list minimal — this is the
/// only place the agent needs it.
fn base64_encode(input: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((input.len() + 2) / 3 * 4);
    for chunk in input.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let n = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
        out.push(T[(n >> 18 & 0x3F) as usize] as char);
        out.push(T[(n >> 12 & 0x3F) as usize] as char);
        out.push(if chunk.len() > 1 { T[(n >> 6 & 0x3F) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { T[(n & 0x3F) as usize] as char } else { '=' });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_matches_the_reference_alphabet() {
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foob"), "Zm9vYg==");
        assert_eq!(base64_encode(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
        // 32 bytes, the actual case: an X25519 public key. 32 % 3 == 2, so a
        // single '=' pad. Cross-checked against Python's base64.b64encode.
        assert_eq!(base64_encode(&[0xFFu8; 32]), "//////////////////////////////////////////8=");
    }

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

    #[test]
    fn signed_id_is_verifiable_and_carries_the_ephemeral_key() {
        let (pk, sk) = sign::gen_keypair();
        let agent = Handshake::new();
        let signed = agent.signed_id("123456789", &sk);
        let opened = sign::verify(&signed, &pk).expect("signature must verify");
        let text = String::from_utf8(opened).unwrap();
        let mut parts = text.splitn(2, '\0');
        assert_eq!(parts.next().unwrap(), "123456789");
        assert_eq!(parts.next().unwrap(), base64_encode(&agent.our_pk_b.0));
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
