//! Just enough zstd to be understood: valid frames that store data uncompressed.
//!
//! `CursorData.colors` is not raw pixels on the wire. The client runs it through
//! `hbb_common::compress::decompress`, which is `zstd::block::Decompressor`, so
//! raw RGBA arrives as garbage, `repng::encode` fails, and no cursor is ever
//! drawn -- silently, because nothing in the protocol reports it.
//!
//! Linking libzstd for this target would be a build problem for the sake of a
//! 912-byte image. It is also unnecessary: a zstd frame may carry **Raw
//! blocks**, which hold literal bytes, so a spec-valid frame can be assembled
//! without implementing any compression at all. A real decoder accepts it and
//! hands back exactly what went in.
//!
//! Frame layout (RFC 8878 §3.1):
//!
//! ```text
//!   Magic_Number              4 bytes, little-endian 0xFD2FB528
//!   Frame_Header_Descriptor   1 byte
//!   [Frame_Content_Size]      1, 2, 4 or 8 bytes
//!   Block…                    3-byte header each, then the literal bytes
//! ```
//!
//! `Single_Segment_flag` is set, which removes the Window_Descriptor and tells
//! the decoder the window is the whole content. No checksum, no dictionary.

/// Largest a single block may be. Above this the payload is split.
const BLOCK_MAX: usize = 128 * 1024;

/// Wrap `data` in a zstd frame of raw blocks.
pub fn raw_frame(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len() + 16);
    out.extend_from_slice(&0xFD2FB528u32.to_le_bytes());

    // Frame_Header_Descriptor: bits 7-6 select the size of the content field,
    // bit 5 is Single_Segment_flag. Everything else stays zero.
    let n = data.len();
    if n < 256 {
        out.push(0b0010_0000);
        out.push(n as u8);
    } else if n < 65_792 {
        out.push(0b0110_0000);
        // A two-byte field is stored biased by 256, so it covers 256..=65791.
        out.extend_from_slice(&((n - 256) as u16).to_le_bytes());
    } else {
        out.push(0b1010_0000);
        out.extend_from_slice(&(n as u32).to_le_bytes());
    }

    if data.is_empty() {
        // Still needs one block, and it has to be flagged as the last.
        out.extend_from_slice(&block_header(0, true));
        return out;
    }

    let mut off = 0;
    while off < n {
        let len = BLOCK_MAX.min(n - off);
        let last = off + len == n;
        out.extend_from_slice(&block_header(len, last));
        out.extend_from_slice(&data[off..off + len]);
        off += len;
    }
    out
}

/// Block_Header is a 24-bit little-endian value: bit 0 marks the last block,
/// bits 1-2 are the block type (0 = Raw), and the rest is the size.
fn block_header(size: usize, last: bool) -> [u8; 3] {
    let v = ((size as u32) << 3) | (0 << 1) | (last as u32);
    [v as u8, (v >> 8) as u8, (v >> 16) as u8]
}

#[cfg(test)]
mod tests {
    use super::*;

    const MAGIC: [u8; 4] = [0x28, 0xB5, 0x2F, 0xFD];

    #[test]
    fn every_frame_starts_with_the_zstd_magic() {
        for len in [0usize, 1, 255, 256, 912, 70_000] {
            let f = raw_frame(&vec![7u8; len]);
            assert_eq!(&f[..4], &MAGIC, "bad magic for a {}-byte payload", len);
        }
    }

    /// The three content-size encodings, including the 256 bias on the middle
    /// one -- getting that wrong makes a decoder expect the wrong length.
    #[test]
    fn the_content_size_field_is_encoded_by_size() {
        let small = raw_frame(&[1, 2, 3]);
        assert_eq!(small[4], 0b0010_0000);
        assert_eq!(small[5], 3);

        let mid = raw_frame(&vec![0u8; 912]);
        assert_eq!(mid[4], 0b0110_0000);
        assert_eq!(u16::from_le_bytes([mid[5], mid[6]]), 912 - 256);

        let big = raw_frame(&vec![0u8; 70_000]);
        assert_eq!(big[4], 0b1010_0000);
        assert_eq!(u32::from_le_bytes([big[5], big[6], big[7], big[8]]), 70_000);
    }

    /// Where the blocks start: 4 magic + 1 descriptor + the content-size field,
    /// whose width the descriptor's top two bits select. Computing this rather
    /// than hardcoding it is the point -- a test that assumes 9 bytes passes for
    /// a 3-byte payload and silently reads a block header as data for a 912-byte
    /// one.
    fn blocks_start(frame: &[u8]) -> usize {
        4 + 1 + match frame[4] >> 6 {
            0 => 1,
            1 => 2,
            2 => 4,
            _ => 8,
        }
    }

    /// Walk the block list, returning (size, is_last, payload) for each.
    fn blocks(frame: &[u8]) -> Vec<(usize, bool, Vec<u8>)> {
        let mut at = blocks_start(frame);
        let mut out = Vec::new();
        loop {
            let hdr = u32::from_le_bytes([frame[at], frame[at + 1], frame[at + 2], 0]);
            let last = hdr & 1 == 1;
            let kind = (hdr >> 1) & 0b11;
            let size = (hdr >> 3) as usize;
            assert_eq!(kind, 0, "every block must be Raw");
            at += 3;
            out.push((size, last, frame[at..at + size].to_vec()));
            at += size;
            if last {
                break;
            }
        }
        assert_eq!(at, frame.len(), "trailing bytes after the last block");
        out
    }

    #[test]
    fn a_small_payload_is_one_raw_block_marked_last() {
        let b = blocks(&raw_frame(&[9, 9, 9, 9]));
        assert_eq!(b.len(), 1);
        assert_eq!(b[0].0, 4);
        assert!(b[0].1, "the only block must be flagged last");
        assert_eq!(b[0].2, vec![9, 9, 9, 9]);
    }

    /// Over 128 KB the payload has to split, and only the final block may carry
    /// the last flag.
    #[test]
    fn a_large_payload_splits_into_blocks() {
        let data = vec![3u8; BLOCK_MAX + 100];
        let b = blocks(&raw_frame(&data));
        assert_eq!(b.len(), 2);
        assert_eq!((b[0].0, b[0].1), (BLOCK_MAX, false));
        assert_eq!((b[1].0, b[1].1), (100, true));
    }

    /// The payload survives byte for byte; a raw block must not transform it.
    #[test]
    fn the_payload_is_carried_verbatim() {
        let data: Vec<u8> = (0..=255u8).cycle().take(912).collect();
        let rebuilt: Vec<u8> =
            blocks(&raw_frame(&data)).into_iter().flat_map(|(_, _, d)| d).collect();
        assert_eq!(rebuilt, data, "raw blocks must not alter the bytes");
    }
}
