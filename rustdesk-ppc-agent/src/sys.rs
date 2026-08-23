//! What the machine actually has.
//!
//! This agent targets a family, not a model: single-processor G4s and G5s are
//! as much a target as the dual G5 it happens to be developed on. Anything that
//! spends a second core has to ask first, at runtime -- a build-time assumption
//! would either waste a processor on the machines that have two or oversubscribe
//! the ones that do not.

/// Processors currently online, at least 1.
///
/// `sysconf` rather than `sysctl`: it takes an int and returns a long, so
/// nothing but scalars crosses the FFI boundary. That matters here — see
/// `input_shim.c` for what happens on 32-bit PowerPC when a struct does.
pub fn cpu_count() -> usize {
    #[cfg(unix)]
    {
        let n = unsafe { libc::sysconf(libc::_SC_NPROCESSORS_ONLN) };
        if n >= 1 {
            return n as usize;
        }
    }
    1
}

/// Wait until a socket has something to read, or `ms` milliseconds pass.
///
/// Exists because IRIX has no `SO_RCVTIMEO`: `set_read_timeout` there fails
/// with ENOPROTOOPT, and the session loop paces itself on a read that is
/// supposed to time out. `poll(2)` does the same job without needing the socket
/// option, and says the same thing -- "nothing arrived" -- without an error.
///
/// `ms` of zero polls and returns immediately, which is what the loop wants
/// between bands.
#[cfg(target_os = "irix")]
pub fn wait_readable(s: &std::net::TcpStream, ms: u64) -> bool {
    use std::os::unix::io::AsRawFd;
    wait_readable_fd(s.as_raw_fd(), ms)
}

/// The same, for anything else with a descriptor.
///
/// The rendezvous loop needs it for a `UdpSocket`, and needs it more than the
/// video loop does: there the missing timeout costs pacing, but the
/// registration loop is *built* on the read timing out -- that is what tells it
/// to resend. Without one, `recv` blocks for ever on the first datagram that
/// never comes and the agent silently stops registering.
#[cfg(target_os = "irix")]
pub fn wait_readable_fd(fd: std::os::unix::io::RawFd, ms: u64) -> bool {
    let mut fds = libc::pollfd {
        fd,
        events: libc::POLLIN,
        revents: 0,
    };
    let n = unsafe { libc::poll(&mut fds, 1, ms as libc::c_int) };
    n > 0 && (fds.revents & libc::POLLIN) != 0
}

/// Threads to give a job that can use more than one, leaving the machine
/// responsive: never more than the cores present, and never more than `cap`.
///
/// A single-processor machine always gets 1, which is the point.
pub fn threads_for(cap: usize) -> usize {
    cpu_count().min(cap).max(1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn there_is_always_at_least_one_processor() {
        assert!(cpu_count() >= 1);
    }

    /// The cap is an upper bound, never a floor -- a single-processor G5 must
    /// come back with 1 however much a caller asks for.
    #[test]
    fn threads_never_exceed_the_cores_present_or_the_cap() {
        let n = cpu_count();
        assert_eq!(threads_for(1), 1);
        assert!(threads_for(2) <= n.min(2).max(1));
        assert!(threads_for(64) <= n);
        assert!(threads_for(0) >= 1, "a nonsense cap must not produce zero threads");
    }
}

/// Turn a `kern.osrelease` string into a Mac OS X (major, minor).
///
/// Darwin 9 is 10.5, Darwin 10 is 10.6, and so on with a constant offset of
/// four. Split out from the sysctl so the arithmetic is testable somewhere
/// other than a Mac.
fn parse_darwin_release(s: &str) -> Option<(u32, u32)> {
    let darwin: u32 = s.trim().split('.').next()?.parse().ok()?;
    if darwin >= 4 {
        Some((10, darwin - 4))
    } else {
        None
    }
}

/// The Mac OS X version, as (major, minor) -- 10.5 on Leopard, 10.6 on Snow
/// Leopard.
///
/// `Gestalt` would be the Carbon way and needs a framework and a struct across
/// the FFI boundary; a sysctl returns a string and nothing else.
///
/// Returns `None` rather than guessing if it cannot be read, so callers decide
/// what an unknown system is allowed to do.
pub fn macos_version() -> Option<(u32, u32)> {
    #[cfg(target_os = "macos")]
    {
        if let Some(s) = sysctl_string(b"kern.osrelease\0") {
            return parse_darwin_release(&s);
        }
    }
    None
}

#[cfg(target_os = "macos")]
extern "C" {
    fn sysctlbyname(
        name: *const std::os::raw::c_char,
        oldp: *mut std::os::raw::c_void,
        oldlenp: *mut usize,
        newp: *mut std::os::raw::c_void,
        newlen: usize,
    ) -> std::os::raw::c_int;
}

/// Bytes written by a sysctl, and how many there were.
#[cfg(target_os = "macos")]
fn sysctl_raw(name: &[u8], buf: &mut [u8]) -> Option<usize> {
    let mut len = buf.len();
    let rc = unsafe {
        sysctlbyname(
            name.as_ptr() as *const _,
            buf.as_mut_ptr() as *mut _,
            &mut len,
            std::ptr::null_mut(),
            0,
        )
    };
    if rc == 0 && len > 0 && len <= buf.len() {
        Some(len)
    } else {
        None
    }
}

/// A string sysctl, without the NUL the kernel counts and the string does not.
#[cfg(target_os = "macos")]
fn sysctl_string(name: &[u8]) -> Option<String> {
    let mut buf = [0u8; 128];
    let len = sysctl_raw(name, &mut buf)?;
    if len < 2 {
        return None;
    }
    Some(String::from_utf8_lossy(&buf[..len - 1]).into_owned())
}

/// An integer sysctl, at whatever width the kernel returns it.
///
/// **The width is not fixed, and the bytes are in the machine's own order.**
/// `hw.memsize` is a uint64 while `hw.physmem` and `hw.cputype` are 32-bit, so
/// the value has to be rebuilt from the length the kernel reports rather than
/// read through one fixed-width type.
///
/// That distinction is not pedantry on this target. The bytes are big-endian,
/// so a 4-byte answer read as though it were 8 lands in the *high* half and
/// comes back multiplied by 2^32 -- a wrong number rather than an error, which
/// is the same shape as the `AddrMangle` bug in BACKLOG §12's neighbourhood.
/// `from_ne_bytes` is deliberate: the kernel writes host order, not network
/// order.
#[cfg(target_os = "macos")]
fn sysctl_u64(name: &[u8]) -> Option<u64> {
    let mut buf = [0u8; 8];
    match sysctl_raw(name, &mut buf)? {
        8 => Some(u64::from_ne_bytes(buf)),
        4 => {
            let mut four = [0u8; 4];
            four.copy_from_slice(&buf[..4]);
            Some(u32::from_ne_bytes(four) as u64)
        }
        _ => None,
    }
}

/// Does this system have the gesture events a touch device would drive?
///
/// Magnify, rotate and swipe arrived with 10.6; on 10.5 there is nothing to
/// post them through, so a peer's touch messages are dropped rather than
/// approximated. Unknown versions are treated as too old, which is the
/// direction that cannot invent input nobody asked for.
pub fn has_gesture_events() -> bool {
    match macos_version() {
        Some((major, minor)) => major > 10 || (major == 10 && minor >= 6),
        None => false,
    }
}

#[cfg(test)]
mod version_tests {
    use super::*;

    #[test]
    fn darwin_releases_map_to_the_system_they_shipped_with() {
        assert_eq!(parse_darwin_release("8.11.0"), Some((10, 4)));  // Tiger
        assert_eq!(parse_darwin_release("9.8.0"), Some((10, 5)));   // Leopard
        assert_eq!(parse_darwin_release("10.8.0"), Some((10, 6)));  // Snow Leopard
        assert_eq!(parse_darwin_release("11.4.2"), Some((10, 7)));  // Lion
        assert_eq!(parse_darwin_release("nonsense"), None);
        assert_eq!(parse_darwin_release(""), None);
    }

    /// The gate this exists for: gestures on Snow Leopard and later, nothing
    /// before it, and nothing at all when the version is unreadable.
    #[test]
    fn gestures_start_at_snow_leopard() {
        let gated = |r: &str| match parse_darwin_release(r) {
            Some((maj, min)) => maj > 10 || (maj == 10 && min >= 6),
            None => false,
        };
        assert!(!gated("9.8.0"), "10.5 has no gesture events");
        assert!(gated("10.8.0"), "10.6 does");
        assert!(gated("11.4.2"));
        assert!(!gated("garbage"), "an unknown system must not claim support");
    }

    /// Whatever this host is, the two must agree with each other.
    #[test]
    fn gesture_support_follows_the_detected_version() {
        match macos_version() {
            Some((10, m)) => assert_eq!(has_gesture_events(), m >= 6),
            Some((maj, _)) => assert_eq!(has_gesture_events(), maj > 10),
            None => assert!(!has_gesture_events()),
        }
    }
}

// ---------------------------------------------------------------------------
// What the console is told about this machine
// ---------------------------------------------------------------------------
//
// `api` posts these three as `cpu`, `memory` and `os`. They are free-form
// display strings: the console stores them and shows them, and reads nothing
// out of them, so the only requirements are that they are honest and that they
// fit on one line of a device list.
//
// Empty is a legal answer for `cpu` and `memory`. Only `hostname` drives the
// console's request to be told again, so a blank here costs a less informative
// row and nothing else -- see `api`'s header for why that matters.

/// `CPU_TYPE_POWERPC`, from `mach/machine.h`.
#[cfg(target_os = "macos")]
const CPU_TYPE_POWERPC: u64 = 18;

/// The name written on the machine, for a `cpu_subtype_t` out of
/// `mach/machine.h`.
///
/// The subtype is a part number and the name is what someone reading a device
/// list is looking for: a 7400 and a 7450 are both a G4. Parts with no
/// marketing name keep their number, and anything unrecognised falls back to
/// the family rather than guessing at a generation.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn powerpc_name(subtype: u64) -> &'static str {
    match subtype {
        1 => "PowerPC 601",
        2 => "PowerPC 602",
        3 | 4 | 5 => "PowerPC 603",
        6 | 7 => "PowerPC 604",
        8 => "PowerPC 620",
        9 => "PowerPC G3",
        10 | 11 => "PowerPC G4",
        100 => "PowerPC G5",
        _ => "PowerPC",
    }
}

/// Assemble the one-line CPU description.
///
/// An unnameable processor yields nothing at all rather than a string of
/// qualifiers with no subject: "@ 2 GHz (2 cores)" tells a reader less than an
/// empty cell does.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn describe_cpu(name: &str, hz: Option<u64>, cores: usize) -> String {
    if name.is_empty() {
        return String::new();
    }
    let mut s = name.to_owned();
    if let Some(hz) = hz.filter(|h| *h > 0) {
        s.push_str(" @ ");
        s.push_str(&format_frequency(hz));
    }
    if cores > 1 {
        s.push_str(&format!(" ({} cores)", cores));
    }
    s
}

/// Hertz as a human reads them: "2 GHz", "1.42 GHz", "867 MHz".
///
/// Two decimal places at most, and no trailing zero -- a 1.42 GHz G4 and a
/// 2 GHz G5 should both look like what is printed on the box.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn format_frequency(hz: u64) -> String {
    const GHZ: u64 = 1_000_000_000;
    const MHZ: u64 = 1_000_000;
    if hz >= GHZ {
        let hundredths = (hz * 100 + GHZ / 2) / GHZ;
        let (whole, frac) = (hundredths / 100, hundredths % 100);
        if frac == 0 {
            format!("{} GHz", whole)
        } else if frac % 10 == 0 {
            format!("{}.{} GHz", whole, frac / 10)
        } else {
            format!("{}.{:02} GHz", whole, frac)
        }
    } else {
        format!("{} MHz", (hz + MHZ / 2) / MHZ)
    }
}

/// Bytes as a machine is advertised: binary units, decimal-looking labels.
///
/// This is what the Apple System Profiler on these machines says, so a device
/// list that agrees with it is the one that will not be questioned.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn format_memory(bytes: u64) -> String {
    const GIB: u64 = 1 << 30;
    const MIB: u64 = 1 << 20;
    if bytes >= GIB {
        let tenths = (bytes * 10 + GIB / 2) / GIB;
        if tenths % 10 == 0 {
            format!("{} GB", tenths / 10)
        } else {
            format!("{}.{} GB", tenths / 10, tenths % 10)
        }
    } else if bytes >= MIB {
        format!("{} MB", (bytes + MIB / 2) / MIB)
    } else {
        format!("{} bytes", bytes)
    }
}

/// The processor, for the console's device list.
#[cfg(target_os = "macos")]
pub fn cpu_description() -> String {
    let name = match (sysctl_u64(b"hw.cputype\0"), sysctl_u64(b"hw.cpusubtype\0")) {
        (Some(CPU_TYPE_POWERPC), Some(sub)) => powerpc_name(sub).to_owned(),
        // Not a PowerPC, so the subtype table does not apply. `hw.model` names
        // the machine rather than the chip, which is the more useful of the
        // two answers available.
        _ => sysctl_string(b"hw.model\0").unwrap_or_default(),
    };
    describe_cpu(&name, sysctl_u64(b"hw.cpufrequency\0"), cpu_count())
}

/// Nothing, off a Mac. The host build exists to test the protocol, and
/// inventing a processor description for it would put fiction in a device list.
#[cfg(not(target_os = "macos"))]
pub fn cpu_description() -> String {
    String::new()
}

/// Installed memory, for the console's device list.
#[cfg(target_os = "macos")]
pub fn memory_description() -> String {
    // `hw.memsize` is a uint64 and is the right answer. `hw.physmem` is a
    // 32-bit int that saturates just under 4 GB, so it is a fallback for a
    // system too old to have the former and never a first choice -- a 8 GB G5
    // read through it reports 4.
    match sysctl_u64(b"hw.memsize\0").or_else(|| sysctl_u64(b"hw.physmem\0")) {
        Some(b) if b > 0 => format_memory(b),
        _ => String::new(),
    }
}

#[cfg(not(target_os = "macos"))]
pub fn memory_description() -> String {
    String::new()
}

/// The operating system, for the console's device list.
#[cfg(target_os = "macos")]
pub fn os_description() -> String {
    match macos_version() {
        Some((major, minor)) => format!("Mac OS X {}.{}", major, minor),
        // The version is unreadable but the platform is not in doubt.
        None => "Mac OS X".to_owned(),
    }
}

/// Off a Mac this is a host test build, and saying so is more use than a blank.
#[cfg(not(target_os = "macos"))]
pub fn os_description() -> String {
    std::env::consts::OS.to_owned()
}

#[cfg(test)]
mod inventory_tests {
    use super::*;

    #[test]
    fn powerpc_subtypes_map_to_the_name_on_the_machine() {
        assert_eq!(powerpc_name(9), "PowerPC G3");
        assert_eq!(powerpc_name(10), "PowerPC G4");
        assert_eq!(powerpc_name(11), "PowerPC G4", "7450 is a G4 as much as 7400 is");
        assert_eq!(powerpc_name(100), "PowerPC G5");
        assert_eq!(powerpc_name(6), "PowerPC 604");
        assert_eq!(powerpc_name(999), "PowerPC", "an unknown part must not be given a generation");
    }

    /// The two machines this agent actually targets.
    #[test]
    fn a_g5_and_a_g4_describe_themselves_the_way_the_box_did() {
        assert_eq!(
            describe_cpu("PowerPC G5", Some(2_000_000_000), 2),
            "PowerPC G5 @ 2 GHz (2 cores)"
        );
        assert_eq!(
            describe_cpu("PowerPC G4", Some(1_420_000_000), 1),
            "PowerPC G4 @ 1.42 GHz",
            "a single processor is not worth a parenthetical"
        );
    }

    #[test]
    fn an_unnameable_processor_describes_nothing_rather_than_qualifiers_alone() {
        assert_eq!(describe_cpu("", Some(2_000_000_000), 2), "");
        assert_eq!(describe_cpu("PowerPC G5", None, 1), "PowerPC G5");
        assert_eq!(describe_cpu("PowerPC G5", Some(0), 1), "PowerPC G5");
    }

    #[test]
    fn frequencies_read_the_way_they_are_advertised() {
        assert_eq!(format_frequency(2_000_000_000), "2 GHz");
        assert_eq!(format_frequency(1_800_000_000), "1.8 GHz");
        assert_eq!(format_frequency(1_420_000_000), "1.42 GHz");
        assert_eq!(format_frequency(867_000_000), "867 MHz");
        assert_eq!(format_frequency(0), "0 MHz");
    }

    #[test]
    fn memory_reads_the_way_the_system_profiler_says_it() {
        assert_eq!(format_memory(8 << 30), "8 GB");
        assert_eq!(format_memory(2 << 30), "2 GB");
        assert_eq!(format_memory((3 << 30) / 2), "1.5 GB");
        assert_eq!(format_memory(512 << 20), "512 MB");
        assert_eq!(format_memory(0), "0 bytes");
    }

    /// Whatever host this runs on, the three have to be safe to post: no
    /// panics, and nothing that would break the JSON they land in.
    #[test]
    fn the_inventory_strings_are_safe_to_send() {
        for s in [cpu_description(), memory_description(), os_description()].iter() {
            assert!(!s.contains('\0'), "a NUL would truncate the request");
            assert!(s.len() < 200, "a device list shows one line");
        }
        assert!(!os_description().is_empty(), "the platform is always knowable");
    }
}
