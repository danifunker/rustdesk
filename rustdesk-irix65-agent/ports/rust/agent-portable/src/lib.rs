//! Compile the agent's portable modules for `mips-sgi-irix6.5`.
//!
//! Sources come from the PPC tree by `#[path]`, deliberately: the point is to
//! find out whether *that* code builds for IRIX, and a copy would answer a
//! question about the copy.

const PPC: &str = "../../../../../rustdesk/rustdesk-ppc-agent/src";

// Declared the way hbb_common declares them so type paths line up with
// upstream's; these are checked-in codegen output, not built here.
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/protos/message.rs"]
#[allow(renamed_and_removed_lints, clippy::all)]
pub mod message_proto;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/protos/rendezvous.rs"]
#[allow(renamed_and_removed_lints, clippy::all)]
pub mod rendezvous_proto;

#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/json.rs"]
pub mod json;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/convert.rs"]
pub mod convert;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/zstd_frame.rs"]
pub mod zstd_frame;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/frame.rs"]
pub mod frame;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/http.rs"]
pub mod http;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/png.rs"]
pub mod png;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/crypto.rs"]
pub mod crypto;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/config.rs"]
pub mod config;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/sys.rs"]
pub mod sys;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/encode.rs"]
pub mod encode;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/cursor.rs"]
pub mod cursor;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/clipboard.rs"]
pub mod clipboard;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/input.rs"]
pub mod input;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/session.rs"]
pub mod session;
// The networking core. None of these three carries a single cfg(macos), which
// is a good sign that the protocol work was kept away from the platform.
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/api.rs"]
pub mod api;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/lan.rs"]
pub mod lan;
#[path = "../../../../../rustdesk/rustdesk-ppc-agent/src/rendezvous.rs"]
pub mod rendezvous;

pub fn ppc_src() -> &'static str { PPC }

// The IRIX capture path. Its C half lives beside it in ../../../src, builds via
// build.rs, and needs Xlib, so it only exists on the target.
#[cfg(target_os = "irix")]
#[path = "../../../../src/capture.rs"]
pub mod capture;
