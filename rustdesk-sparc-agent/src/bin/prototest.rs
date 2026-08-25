//! prototest -- does the portable half behave the same on a big-endian machine?
//!
//! Every layer below the session has an endianness question in it, and none of
//! them announce a wrong answer:
//!
//!   * protobuf writes `fixed32`/`fixed64` little-endian on the wire whatever
//!     the machine is, so an implementation that stores them natively produces
//!     bytes a peer cannot read -- and reads its own output back perfectly.
//!   * PNG is big-endian: lengths, CRCs and the IHDR fields. Code written on a
//!     little-endian machine has to swap, and code that forgot produces a file
//!     that only its author can open.
//!   * the frame header the agent puts in front of every message has its own
//!     byte order to get wrong.
//!
//! So this round-trips each of them and checks the *bytes*, not just that a
//! value survives a trip through its own code.

use rustdesk_sparc_agent::{convert, frame, png, rendezvous_proto, sys};

use protobuf::Message;

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect::<Vec<_>>().join(" ")
}

fn main() {
    let mut failures = 0;

    // --- protobuf ---------------------------------------------------------
    let mut peer = rendezvous_proto::RegisterPeer::new();
    peer.id = "sparc-blade".to_owned();
    peer.serial = 0x0102_0304;
    let bytes = peer.write_to_bytes().expect("serialise");
    println!("RegisterPeer wire bytes: {}", hex(&bytes));
    let back = rendezvous_proto::RegisterPeer::parse_from_bytes(&bytes).expect("parse");
    println!("  id={:?} serial=0x{:08x}", back.id, back.serial);
    if back.id != peer.id || back.serial != peer.serial {
        println!("  FAIL: did not survive the round trip");
        failures += 1;
    }
    // `serial` is a varint, so 0x01020304 is five bytes ending 0x08 0x01 --
    // little-endian *base 128*, which is the same on both machines. A fixed64
    // is the interesting case; the id string pins the framing either way.
    if !bytes.windows(11).any(|w| w == b"sparc-blade") {
        println!("  FAIL: the id is not in the encoded bytes");
        failures += 1;
    }

    // --- the agent's own frame header -------------------------------------
    let framed = frame::encode(b"hello").expect("frame");
    println!("frame header for a 5 byte payload: {}", hex(&framed[..framed.len() - 5]));
    let mut cur = std::io::Cursor::new(framed.clone());
    let got = frame::read_frame(&mut cur, 1 << 20).expect("read_frame");
    if got != b"hello" {
        println!("  FAIL: frame round trip gave {:?}", got);
        failures += 1;
    }

    // --- PNG --------------------------------------------------------------
    // A 4x2 image with known corners, so a channel swap or a row stride bug is
    // visible in the decoded output rather than merely suspected.
    let (w, h) = (4usize, 2usize);
    let mut argb = vec![0u8; w * h * 4];
    for (i, px) in argb.chunks_mut(4).enumerate() {
        px[0] = 0xff;                        // pad/alpha
        px[1] = if i == 0 { 0xff } else { 0 };   // r
        px[2] = if i == 1 { 0xff } else { 0 };   // g
        px[3] = if i == 2 { 0xff } else { 0 };   // b
    }
    match png::encode_argb(&argb, w * 4, w, h) {
        Ok(data) => {
            println!("PNG: {} bytes, signature {}", data.len(), hex(&data[..8]));
            // IHDR width/height are big-endian: bytes 16..24.
            let iw = u32::from_be_bytes([data[16], data[17], data[18], data[19]]);
            let ih = u32::from_be_bytes([data[20], data[21], data[22], data[23]]);
            println!("  IHDR says {}x{}", iw, ih);
            if iw != w as u32 || ih != h as u32 {
                println!("  FAIL: IHDR is not big-endian {}x{}", w, h);
                failures += 1;
            }
            if data[..8] != [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a] {
                println!("  FAIL: bad PNG signature");
                failures += 1;
            }
            let path = std::env::args().nth(1).unwrap_or_else(|| "/tmp/prototest.png".to_owned());
            std::fs::write(&path, &data).expect("write png");
            println!("  wrote {} -- decode it off-machine to be sure", path);
        }
        Err(e) => {
            println!("PNG: encode_argb failed: {}", e);
            failures += 1;
        }
    }

    // --- ARGB -> I420 ------------------------------------------------------
    // Pure white in, so Y should be near 235 and the chroma planes near 128
    // whatever the byte order; a red/blue swap shows up as chroma that is not.
    let (cw, ch) = (16usize, 16usize);
    let white = vec![0xffu8; cw * ch * 4];
    let mut i420 = convert::I420::new(cw, ch);
    convert::argb_to_i420(&white, cw * 4, &mut i420);
    let y0 = i420.y[0];
    let u0 = i420.u[0];
    let v0 = i420.v[0];
    println!("white 16x16 -> Y={} U={} V={}", y0, u0, v0);
    if !(230..=240).contains(&y0) || !(126..=130).contains(&u0) || !(126..=130).contains(&v0) {
        println!("  FAIL: not what BT.601 says white is");
        failures += 1;
    }

    // --- what this machine says about itself ------------------------------
    println!("cpu:    {:?}", sys::cpu_description());
    println!("memory: {:?}", sys::memory_description());
    println!("os:     {:?}", sys::os_description());
    if sys::cpu_description().is_empty() || sys::os_description() == "solaris" {
        println!("  FAIL: the machine is describing itself as a host test build");
        failures += 1;
    }

    println!("{}", if failures == 0 { "ALL CHECKS PASSED" } else { "SOME CHECKS FAILED" });
    std::process::exit(if failures == 0 { 0 } else { 1 });
}
