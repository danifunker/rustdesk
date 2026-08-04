//! RustDesk's length-prefixed framing, on blocking `std::io` instead of
//! `tokio_util::codec`.
//!
//! Faithful reimplementation of `libs/hbb_common/src/bytes_codec.rs`. The header
//! is 1-4 bytes little-endian; its low two bits carry `header_len - 1`, and the
//! payload length is the whole value `>> 2`:
//!
//! | payload len | header | encoding                    |
//! |-------------|--------|-----------------------------|
//! | <= 0x3F     | 1 byte | `len << 2 \| 0`             |
//! | <= 0x3FFF   | 2 (LE) | `len << 2 \| 1`             |
//! | <= 0x3FFFFF | 3 (LE) | `len << 2 \| 2`             |
//! | <= 0x3FFFFFFF | 4 (LE) | `len << 2 \| 3`           |
//!
//! The tests at the bottom are ported from upstream's, so the two encoders are
//! checked against the same vectors.

use std::io::{self, Read, Write};

/// Refuse absurd frames rather than trying to allocate them. Upstream defaults
/// to `usize::MAX` and lets callers lower it; on a 32-bit G4/G5 with maybe 1-4 GB
/// an unbounded reserve is a crash, not an error, so this one has a real default.
pub const DEFAULT_MAX_PACKET: usize = 64 << 20; // 64 MiB

pub fn encode_header(len: usize, out: &mut Vec<u8>) -> io::Result<()> {
    if len <= 0x3F {
        out.push((len << 2) as u8);
    } else if len <= 0x3FFF {
        let h = ((len << 2) as u16) | 0x1;
        out.extend_from_slice(&h.to_le_bytes());
    } else if len <= 0x3F_FFFF {
        let h = ((len << 2) as u32) | 0x2;
        out.extend_from_slice(&(h as u16).to_le_bytes());
        out.push((h >> 16) as u8);
    } else if len <= 0x3FFF_FFFF {
        let h = ((len << 2) as u32) | 0x3;
        out.extend_from_slice(&h.to_le_bytes());
    } else {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "Overflow"));
    }
    Ok(())
}

/// Frame `payload` (header + body) into a fresh buffer.
pub fn encode(payload: &[u8]) -> io::Result<Vec<u8>> {
    let mut out = Vec::with_capacity(payload.len() + 4);
    encode_header(payload.len(), &mut out)?;
    out.extend_from_slice(payload);
    Ok(out)
}

/// Read exactly one frame, blocking until it is complete.
pub fn read_frame<R: Read>(r: &mut R, max_packet: usize) -> io::Result<Vec<u8>> {
    let mut first = [0u8; 1];
    r.read_exact(&mut first)?;
    let head_len = ((first[0] & 0x3) + 1) as usize;

    let mut head = [0u8; 4];
    head[0] = first[0];
    if head_len > 1 {
        r.read_exact(&mut head[1..head_len])?;
    }
    let mut n = head[0] as usize;
    if head_len > 1 {
        n |= (head[1] as usize) << 8;
    }
    if head_len > 2 {
        n |= (head[2] as usize) << 16;
    }
    if head_len > 3 {
        n |= (head[3] as usize) << 24;
    }
    n >>= 2;

    if n > max_packet {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Too big packet",
        ));
    }
    let mut buf = vec![0u8; n];
    r.read_exact(&mut buf)?;
    Ok(buf)
}

pub fn write_frame<W: Write>(w: &mut W, payload: &[u8]) -> io::Result<()> {
    w.write_all(&encode(payload)?)?;
    w.flush()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    /// Round-trip at each header-width boundary. Sizes and expected framed
    /// lengths are taken from upstream's bytes_codec tests.
    fn roundtrip(len: usize, expect_total: usize, fill: u8) {
        let payload = vec![fill; len];
        let framed = encode(&payload).unwrap();
        assert_eq!(framed.len(), expect_total, "framed size for len={}", len);
        let mut c = Cursor::new(framed);
        let got = read_frame(&mut c, DEFAULT_MAX_PACKET).unwrap();
        assert_eq!(got.len(), len);
        if len > 0 {
            assert_eq!(got[0], fill);
        }
    }

    #[test]
    fn header_width_boundaries() {
        roundtrip(0, 1, 0);
        roundtrip(0x3F - 1, 0x3F + 1 - 1, 3);
        roundtrip(0x3F, 0x3F + 1, 1);
        roundtrip(0x3F + 1, 0x3F + 1 + 2, 2);
        roundtrip(0x3FFF, 0x3FFF + 2, 4);
        roundtrip(0x3F_FFFF, 0x3F_FFFF + 3, 5);
        roundtrip(0x3F_FFFF + 1, 0x3F_FFFF + 1 + 4, 6);
    }

    #[test]
    fn two_frames_back_to_back() {
        let mut buf = Vec::new();
        buf.extend_from_slice(&encode(b"").unwrap());
        buf.extend_from_slice(&encode(&vec![2u8; 0x3F + 1]).unwrap());
        assert_eq!(buf.len(), 1 + (0x3F + 1 + 2));
        let mut c = Cursor::new(buf);
        assert_eq!(read_frame(&mut c, DEFAULT_MAX_PACKET).unwrap().len(), 0);
        let second = read_frame(&mut c, DEFAULT_MAX_PACKET).unwrap();
        assert_eq!(second.len(), 0x3F + 1);
        assert_eq!(second[0], 2);
    }

    #[test]
    fn oversized_frame_is_rejected_not_allocated() {
        // 0x3FFFFFFF payload announced, tiny cap: must error, never reserve.
        let mut header = Vec::new();
        encode_header(0x3FFF_FFFF, &mut header).unwrap();
        let mut c = Cursor::new(header);
        let err = read_frame(&mut c, 1024).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    #[test]
    fn payload_too_large_to_encode() {
        assert!(encode_header(0x4000_0000, &mut Vec::new()).is_err());
    }
}
