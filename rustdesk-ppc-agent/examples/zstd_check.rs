//! Write frames from `zstd_frame` to disk so a real zstd decoder can verify
//! them. The unit tests check the structure; this checks the claim that
//! matters -- that an actual decompressor accepts what we emit.
fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| ".".into());
    for (name, data) in [
        ("empty", vec![]),
        ("tiny", vec![7u8; 3]),
        ("cursor", (0..=255u8).cycle().take(912).collect::<Vec<u8>>()),
        ("big", vec![42u8; 200_000]),
    ] {
        let frame = rustdesk_ppc_agent::zstd_frame::raw_frame(&data);
        std::fs::write(format!("{}/{}.zst", dir, name), &frame).unwrap();
        std::fs::write(format!("{}/{}.expected", dir, name), &data).unwrap();
        println!("{}: {} bytes in, {} bytes framed", name, data.len(), frame.len());
    }
}
