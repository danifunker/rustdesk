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
