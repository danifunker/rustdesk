// Compiles the libvpx shim. cc-rs picks up CC_<triple> the same way mrustc does,
// so the PowerPC build routes through ppc-cc-remote.py automatically.
//
// The vpx headers are vendored under vpx-include/ rather than referenced on the
// Mac: ppc-cc-remote.py mirrors a local -I directory to the target, but passes
// paths under a *system* prefix through untouched -- and ~/ppc-libs is neither.
fn main() {
    // Only the target build links libvpx; host `cargo test` skips the shim so
    // the pure-Rust modules stay testable without a PowerPC libvpx present.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        println!("cargo:rustc-cfg=no_vpx");
        return;
    }
    cc::Build::new()
        .file("src/vpx_shim.c")
        .include("vpx-include")
        .opt_level(2)
        .compile("vpxshim");
    println!("cargo:rustc-link-lib=static=vpx");
    println!("cargo:rerun-if-changed=src/vpx_shim.c");

    // Quartz injection. In C because CGPoint crosses the API by value, and a
    // 16-byte two-double struct is where the 32-bit PowerPC calling convention
    // diverges from a naive extern "C" declaration.
    cc::Build::new()
        .file("src/input_shim.c")
        .opt_level(2)
        .compile("inputshim");
    println!("cargo:rerun-if-changed=src/input_shim.c");
}
