// Smallest thing that proves std works on the target: it has to allocate, use
// a collection, format, touch the filesystem and read the environment, since
// those are the four places IRIX support most often falls over.
use std::collections::HashMap;

fn main() {
    let mut m: HashMap<String, i32> = HashMap::new();
    for (i, w) in "the quick brown fox".split_whitespace().enumerate() {
        m.insert(w.to_string(), i as i32);
    }
    let mut keys: Vec<&String> = m.keys().collect();
    keys.sort();
    println!("hello from IRIX n32; {} words: {:?}", m.len(), keys);
    println!("args: {:?}", std::env::args().collect::<Vec<_>>());
    match std::env::var("HOME") {
        Ok(h) => println!("HOME={}", h),
        Err(e) => println!("HOME unreadable: {}", e),
    }
    match std::fs::metadata("/etc/passwd") {
        Ok(md) => println!("/etc/passwd is {} bytes", md.len()),
        Err(e) => println!("/etc/passwd stat failed: {}", e),
    }
    println!("current_exe: {:?}", std::env::current_exe());
}
