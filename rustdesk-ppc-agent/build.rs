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

    // Where libvpx, libsodium and libzstd are. `build-ppc.sh` also puts this on
    // the link line via PPC_LDFLAGS, so the cross build does not need it -- but
    // a native build (MacPorts, where the libraries are under ${prefix}/lib)
    // has no wrapper adding -L, and the link then fails on -lvpx. Emitting it
    // here serves both: the remote cc wrapper passes system-prefix paths
    // through untouched, so the same flag is correct on either side.
    println!("cargo:rerun-if-env-changed=PPC_LIBS_DIR");
    if let Ok(dir) = std::env::var("PPC_LIBS_DIR") {
        println!("cargo:rustc-link-search=native={}", dir);
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
