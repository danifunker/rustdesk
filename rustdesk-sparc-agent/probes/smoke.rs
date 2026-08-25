//! smoke.rs -- the first Rust program to run on this machine, and a test of the
//! parts of libstd a Solaris 10 port is most likely to get wrong.
//!
//! Each section exercises something that could plausibly be broken on a
//! big-endian sparcv9 target built through mrustc, rather than anything about
//! Rust itself:
//!
//!   * threads + atomics -- SPARCv9 has `cas`/`casx` for 4 and 8 bytes; the 1-
//!     and 2-byte operations are synthesised by gcc from a word-sized CAS loop.
//!     If that synthesis is wrong, an AtomicU8 counter loses increments.
//!   * Mutex/Condvar -- Solaris threads under the hood.
//!   * catch_unwind -- the DWARF personality routine and the unwinder, which is
//!     the part of a fresh target most likely to abort instead of unwinding.
//!   * TcpListener -- proves the libsocket/libnsl split linked correctly.
//!   * SystemTime/Instant -- clock_gettime on Solaris 10.
//!   * byte order -- printed, not assumed.
//!
//! Build:
//!   bin/mrustc probes/smoke.rs -o smoke -L output-1.74.0-sparcv9-sun-solaris \
//!              --target sparcv9-sun-solaris -O

use std::sync::atomic::{AtomicU8, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

fn main() {
    println!("os={} arch={} pointer_width={}",
             std::env::consts::OS, std::env::consts::ARCH,
             std::mem::size_of::<usize>() * 8);

    // Byte order, measured rather than assumed.
    let n: u32 = 0x01020304;
    let bytes = n.to_ne_bytes();
    println!("native bytes of 0x01020304 = {:02x?} ({})", bytes,
             if bytes[0] == 0x01 { "big-endian" } else { "little-endian" });

    // 128-bit arithmetic: gcc's __int128 on sparcv9, 16-byte aligned.
    let big: u128 = u128::MAX / 3;
    println!("u128 align={} big/7={}", std::mem::align_of::<u128>(), big / 7);

    // Sub-word atomics, the ones gcc has to synthesise.
    let a8 = Arc::new(AtomicU8::new(0));
    let a64 = Arc::new(AtomicU64::new(0));
    let counter = Arc::new(Mutex::new(0u64));
    let mut handles = Vec::new();
    for _ in 0..4 {
        let (a8, a64, counter) = (a8.clone(), a64.clone(), counter.clone());
        handles.push(thread::spawn(move || {
            for _ in 0..250 {
                a8.fetch_add(1, Ordering::SeqCst);
                a64.fetch_add(1, Ordering::SeqCst);
                *counter.lock().unwrap() += 1;
            }
        }));
    }
    for h in handles { h.join().unwrap(); }
    println!("atomics: u8={} (expect 232 after wrap) u64={} (expect 1000) mutex={} (expect 1000)",
             a8.load(Ordering::SeqCst), a64.load(Ordering::SeqCst),
             *counter.lock().unwrap());

    // Unwinding. If the personality routine is wrong this aborts instead.
    let caught = std::panic::catch_unwind(|| {
        panic!("this panic is expected");
    });
    println!("catch_unwind: {}", if caught.is_err() { "unwound OK" } else { "NO PANIC?" });

    // Clocks.
    let t0 = Instant::now();
    thread::sleep(std::time::Duration::from_millis(120));
    let epoch = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
    println!("slept {:?}; unix time {}s", t0.elapsed(), epoch.as_secs());

    // Sockets -- the libsocket/libnsl split.
    match std::net::TcpListener::bind("127.0.0.1:0") {
        Ok(l) => println!("bound {}", l.local_addr().unwrap()),
        Err(e) => println!("bind FAILED: {}", e),
    }

    // Filesystem + strings through the allocator.
    let tmp = std::env::temp_dir().join("smoke-rs.txt");
    std::fs::write(&tmp, "written by rust on solaris\n").unwrap();
    let back = std::fs::read_to_string(&tmp).unwrap();
    std::fs::remove_file(&tmp).unwrap();
    print!("file round-trip: {}", back);

    let mut v: Vec<String> = (0..8).map(|i| format!("item{}", 7 - i)).collect();
    v.sort();
    println!("sorted: {}", v.join(","));

    println!("ALL SECTIONS COMPLETED");
}
