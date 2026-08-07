// Compiles the libvpx shim. cc-rs picks up CC_<triple> the same way mrustc does,
// so the PowerPC build routes through ppc-cc-remote.py automatically.
//
// The vpx headers are vendored under vpx-include/ rather than referenced on the
// Mac: ppc-cc-remote.py mirrors a local -I directory to the target, but passes
// paths under a *system* prefix through untouched -- and ~/ppc-libs is neither.
/// The one -O2 pass that miscompiles on this toolchain.
///
/// `gcc10-bootstrap` for `powerpc-apple-darwin` generates a faulting access for
/// ordinary double arithmetic over file-scope statics at -O2, and only at -O2:
/// -O0, -O1 and -Os are all fine, and of the passes -O2 adds, disabling global
/// common subexpression elimination is the one that avoids it. It cost a live
/// agent, on the first click a user made after a deploy -- see
/// docs/BACKLOG.md.
///
/// Narrower than dropping the shims to -O1, which is what the alternative was.
/// Everything else -O2 offers is kept.
const NO_MISCOMPILE: &str = "-fno-gcse";

fn main() {
    // Only the target build links libvpx; host `cargo test` skips the shim so
    // the pure-Rust modules stay testable without a PowerPC libvpx present.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        println!("cargo:rustc-cfg=no_vpx");
        return;
    }
    cc::Build::new()
        .file("src/vpx_shim.c")
        .flag(NO_MISCOMPILE)
        .include("vpx-include")
        .opt_level(2)
        .compile("vpxshim");
    println!("cargo:rustc-link-lib=static=vpx");
    println!("cargo:rerun-if-changed=src/vpx_shim.c");

    // ARGB -> I420. In C purely for the optimiser: mrustc emits C at -O1 with
    // Rust's bounds checks intact, and this is the hottest arithmetic loop in
    // the agent. src/convert.rs keeps the reference implementation.
    cc::Build::new()
        .file("src/convert_shim.c")
        .flag(NO_MISCOMPILE)
        .opt_level(2)
        .compile("convertshim");
    println!("cargo:rerun-if-changed=src/convert_shim.c");

    // The system cursor image, via private CGS calls -- see the shim's header.
    cc::Build::new()
        .file("src/cursor_shim.c")
        .flag(NO_MISCOMPILE)
        .opt_level(2)
        .compile("cursorshim");
    println!("cargo:rerun-if-changed=src/cursor_shim.c");

    // The Mac clipboard. In C because the Pasteboard Manager is a
    // CoreFoundation API and CFDataRef lifetimes are less ceremony here.
    cc::Build::new()
        .file("src/clipboard_shim.c")
        .flag(NO_MISCOMPILE)
        .opt_level(2)
        .compile("clipboardshim");
    println!("cargo:rerun-if-changed=src/clipboard_shim.c");

    // Opus. The encoder is four calls, and going through `magnum-opus` would
    // add a bindgen build step that mrustc has to transpile for the host before
    // the agent compiles at all. Headers vendored for the same reason vpx's are.
    cc::Build::new()
        .file("src/opus_shim.c")
        .flag(NO_MISCOMPILE)
        .include("opus-include")
        .opt_level(2)
        .compile("opusshim");
    println!("cargo:rustc-link-lib=static=opus");
    println!("cargo:rerun-if-changed=src/opus_shim.c");

    // Sound capture. In C because AUHAL wants a real-time callback and an
    // AudioBufferList, and because 10.5 predates AudioComponentFindNext -- the
    // Component Manager is the only way to a HAL unit here.
    cc::Build::new()
        .file("src/audio_shim.c")
        .flag(NO_MISCOMPILE)
        .opt_level(2)
        .compile("audioshim");
    println!("cargo:rerun-if-changed=src/audio_shim.c");

    // Quartz injection. In C because CGPoint crosses the API by value, and a
    // 16-byte two-double struct is where the 32-bit PowerPC calling convention
    // diverges from a naive extern "C" declaration.
    cc::Build::new()
        .file("src/input_shim.c")
        .flag(NO_MISCOMPILE)
        .opt_level(2)
        .compile("inputshim");
    println!("cargo:rerun-if-changed=src/input_shim.c");
}
