//! The RustDesk agent for Solaris 10 / SPARC.
//!
//! The platform layer lives here; everything portable comes from the PowerPC
//! tree by `#[path]`, the way the IRIX port takes it. That is deliberate: the
//! point is to build the code that ships, and a copy would answer a question
//! about the copy. Modules arrive in dependency order as each one's external
//! libraries are proved out on this machine.

const PPC: &str = "../../rustdesk-ppc-agent/src";

// Checked-in codegen, declared the way hbb_common declares it so type paths
// line up with upstream's.
#[path = "../../rustdesk-ppc-agent/src/protos/message.rs"]
#[allow(renamed_and_removed_lints, clippy::all)]
pub mod message_proto;
#[path = "../../rustdesk-ppc-agent/src/protos/rendezvous.rs"]
#[allow(renamed_and_removed_lints, clippy::all)]
pub mod rendezvous_proto;

#[path = "../../rustdesk-ppc-agent/src/json.rs"]
pub mod json;
#[path = "../../rustdesk-ppc-agent/src/convert.rs"]
pub mod convert;
#[path = "../../rustdesk-ppc-agent/src/png.rs"]
pub mod png;
#[path = "../../rustdesk-ppc-agent/src/frame.rs"]
pub mod frame;
#[path = "../../rustdesk-ppc-agent/src/zstd_frame.rs"]
pub mod zstd_frame;
#[path = "../../rustdesk-ppc-agent/src/lan.rs"]
pub mod lan;

// Still out: config and crypto want libsodium, http wants mbedTLS, encode
// wants libvpx, and session wants all of them.

/// Screen capture over MIT-SHM, with DAMAGE deciding what to report.
#[cfg(target_os = "solaris")]
pub mod capture;

pub fn ppc_src() -> &'static str { PPC }
