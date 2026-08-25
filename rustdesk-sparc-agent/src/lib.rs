//! The RustDesk agent for Solaris 10 / SPARC.
//!
//! Only the platform layer lives here. The portable modules -- protocol,
//! crypto, session, config -- come from the PowerPC tree by `#[path]` as they
//! are proved out on this target, the way the IRIX port takes them, so that
//! what is built here is the code that ships rather than a copy of it.

/// Screen capture over MIT-SHM, with DAMAGE deciding what to report.
#[cfg(target_os = "solaris")]
pub mod capture;
