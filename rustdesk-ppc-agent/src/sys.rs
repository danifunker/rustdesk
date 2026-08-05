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
        extern "C" {
            fn sysctlbyname(
                name: *const std::os::raw::c_char,
                oldp: *mut std::os::raw::c_void,
                oldlenp: *mut usize,
                newp: *mut std::os::raw::c_void,
                newlen: usize,
            ) -> std::os::raw::c_int;
        }
        let mut buf = [0u8; 64];
        let mut len = buf.len();
        let rc = unsafe {
            sysctlbyname(
                b"kern.osrelease\0".as_ptr() as *const _,
                buf.as_mut_ptr() as *mut _,
                &mut len,
                std::ptr::null_mut(),
                0,
            )
        };
        if rc == 0 && len > 1 {
            let s = String::from_utf8_lossy(&buf[..len - 1]).to_string();
            return parse_darwin_release(&s);
        }
    }
    None
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
