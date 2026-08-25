//! The RustDesk agent for Solaris 10 / SPARC.
//!
//! The platform layer lives here; everything portable comes from the PowerPC
//! tree by `#[path]`, the way the IRIX port takes it. That is deliberate: the
//! point is to build the code that ships, and a copy would answer a question
//! about the copy.
//!
//! All of it is wired now, and the agent built from it serves a peer. The
//! modules arrived in dependency order as each one's external libraries were
//! proved out on this machine; `capture` is the one exception, and says below
//! why it is this directory's own.

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
#[path = "../../rustdesk-ppc-agent/src/sys.rs"]
pub mod sys;
#[path = "../../rustdesk-ppc-agent/src/crypto.rs"]
pub mod crypto;
#[path = "../../rustdesk-ppc-agent/src/config.rs"]
pub mod config;
#[path = "../../rustdesk-ppc-agent/src/input.rs"]
pub mod input;
#[path = "../../rustdesk-ppc-agent/src/cursor.rs"]
pub mod cursor;
#[path = "../../rustdesk-ppc-agent/src/http.rs"]
pub mod http;
#[path = "../../rustdesk-ppc-agent/src/encode.rs"]
pub mod encode;


/// Screen capture over MIT-SHM, with DAMAGE deciding what to report.
///
/// The only module here that is this port's own rather than the PowerPC
/// tree's: what a capturer *is* differs per platform, and the shared
/// `session.rs` is written against the shape rather than the implementation.
#[cfg(target_os = "solaris")]
pub mod capture;

// The session, and the three modules it reaches for. These arrived last
// because `session.rs` calls four capture methods -- `invalidate_band`,
// `read_band`, `refresh` and the fused `to_i420_rect` -- that this port did not
// have until `capture_shim.c` grew an A,R,G,B inner loop of its own. See
// `convtest`, which checks that loop against `convert::argb_to_i420` and
// against colours whose BT.601 values are written out, because the way it
// would be wrong is a red/blue swap that looks like a fault in the client.
#[path = "../../rustdesk-ppc-agent/src/clipboard.rs"]
pub mod clipboard;
#[path = "../../rustdesk-ppc-agent/src/api.rs"]
pub mod api;
#[path = "../../rustdesk-ppc-agent/src/rendezvous.rs"]
pub mod rendezvous;
#[path = "../../rustdesk-ppc-agent/src/session.rs"]
pub mod session;

pub fn ppc_src() -> &'static str { PPC }
