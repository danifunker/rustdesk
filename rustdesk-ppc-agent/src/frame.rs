//! RustDesk's length-prefixed framing, on blocking `std::io` instead of
//! `tokio_util::codec`.
//!
//! Faithful reimplementation of `libs/vintage_common/src/bytes_codec.rs`. The header
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

/// A read timeout looks like one of these; the socket sets one so the caller
/// can poll for input without blocking forever.
fn is_timeout(e: &io::Error) -> bool {
    matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut)
}

/// How long to keep waiting for the rest of a frame whose first byte arrived.
/// Only reached if a peer sends a header and then stops, which is a dead
/// connection rather than an idle one.
const FRAME_DEADLINE: std::time::Duration = std::time::Duration::from_secs(10);

/// Read exactly `buf.len()` bytes, *ignoring* read timeouts.
///
/// `read_exact` cannot be used once a frame has started. The socket carries a
/// short read timeout so the caller can poll, and a timeout that lands
/// mid-frame makes `read_exact` fail having already consumed bytes it cannot
/// give back. The next read then takes the middle of that message for a header,
/// and every frame after it is garbage -- a desync that ends the session rather
/// than reporting anything useful.
fn read_rest<R: Read>(r: &mut R, buf: &mut [u8]) -> io::Result<()> {
    let start = std::time::Instant::now();
    let mut off = 0;
    while off < buf.len() {
        match r.read(&mut buf[off..]) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "peer closed part way through a frame",
                ))
            }
            Ok(n) => {
                off += n;
                continue;
            }
            Err(ref e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(ref e) if is_timeout(e) => {
                if start.elapsed() > FRAME_DEADLINE {
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "frame never finished arriving",
                    ));
                }
                continue;
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// Read exactly one frame.
///
/// A timeout on the *first* byte is passed through, since that is how a caller
/// asks "is there anything to read?". After that the frame is seen through to
/// the end -- see `read_rest`.
pub fn read_frame<R: Read>(r: &mut R, max_packet: usize) -> io::Result<Vec<u8>> {
    let mut first = [0u8; 1];
    match r.read(&mut first) {
        Ok(0) => return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "peer closed")),
        Ok(_) => {}
        Err(e) => return Err(e),
    }
    let head_len = ((first[0] & 0x3) + 1) as usize;

    let mut head = [0u8; 4];
    head[0] = first[0];
    if head_len > 1 {
        read_rest(r, &mut head[1..head_len])?;
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
    read_rest(r, &mut buf)?;
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

    /// A reader that hands out a few bytes at a time and reports a timeout in
    /// between, which is exactly what a socket with a read timeout does when a
    /// message arrives split across TCP segments.
    struct Stuttering {
        data: Vec<u8>,
        pos: usize,
        chunk: usize,
        stall_next: bool,
    }

    impl Read for Stuttering {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if self.stall_next && self.pos < self.data.len() {
                self.stall_next = false;
                return Err(io::Error::new(io::ErrorKind::WouldBlock, "timed out"));
            }
            self.stall_next = true;
            if self.pos >= self.data.len() {
                return Ok(0);
            }
            let n = self.chunk.min(buf.len()).min(self.data.len() - self.pos);
            buf[..n].copy_from_slice(&self.data[self.pos..self.pos + n]);
            self.pos += n;
            Ok(n)
        }
    }

    /// How the session loop uses this: a timeout with nothing read means "no
    /// input right now", so the caller comes back later. Timeouts *inside* a
    /// frame are not the caller's business and must never surface here.
    fn read_polling<R: Read>(r: &mut R) -> io::Result<Vec<u8>> {
        loop {
            match read_frame(r, DEFAULT_MAX_PACKET) {
                Err(ref e) if is_timeout(e) => continue,
                other => return other,
            }
        }
    }

    /// The bug this guards: a timeout after the header byte used to abandon the
    /// frame with bytes already consumed, so the next read parsed the middle of
    /// a message as a header and every frame after it was garbage.
    #[test]
    fn a_timeout_part_way_through_does_not_lose_the_frame() {
        let payload: Vec<u8> = (0..200u8).collect();
        let framed = encode(&payload).unwrap();
        let mut r = Stuttering { data: framed, pos: 0, chunk: 3, stall_next: true };
        let got = read_polling(&mut r).expect("frame should survive timeouts");
        assert_eq!(got, payload);
    }

    /// Two frames back to back stay aligned even when every read stutters.
    #[test]
    fn back_to_back_frames_stay_aligned_through_timeouts() {
        let a: Vec<u8> = vec![1u8; 100];
        let b: Vec<u8> = vec![2u8; 5];
        let mut data = encode(&a).unwrap();
        data.extend_from_slice(&encode(&b).unwrap());
        let mut r = Stuttering { data, pos: 0, chunk: 7, stall_next: false };
        assert_eq!(read_polling(&mut r).unwrap(), a);
        assert_eq!(read_polling(&mut r).unwrap(), b, "the second frame must still be aligned");
    }

    /// A timeout with nothing read at all is how the caller polls, so it must
    /// still be reported rather than swallowed.
    #[test]
    fn a_timeout_before_any_byte_is_reported() {
        struct Idle;
        impl Read for Idle {
            fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
                Err(io::Error::new(io::ErrorKind::WouldBlock, "idle"))
            }
        }
        let err = read_frame(&mut Idle, DEFAULT_MAX_PACKET).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::WouldBlock);
    }

    #[test]
    fn a_peer_that_vanishes_mid_frame_is_an_eof_not_a_hang() {
        struct Truncated(Vec<u8>, usize);
        impl Read for Truncated {
            fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
                if self.1 >= self.0.len() {
                    return Ok(0);
                }
                let n = 1.min(buf.len());
                buf[..n].copy_from_slice(&self.0[self.1..self.1 + n]);
                self.1 += n;
                Ok(n)
            }
        }
        let framed = encode(&vec![9u8; 50]).unwrap();
        let mut r = Truncated(framed[..10].to_vec(), 0);
        let err = read_frame(&mut r, DEFAULT_MAX_PACKET).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::UnexpectedEof);
    }
}
