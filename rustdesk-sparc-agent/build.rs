// Compile the Solaris platform shims. They only exist on the target: there is
// no Xsun to link against on the build host, and the X libraries live at paths
// that mean something only over there.
//
// Two prefixes, not one. Xlib, Xext and Xtst are under /usr/openwin/lib, but
// Xfixes and Xdamage are under /usr/openwin/sfw/lib -- with one shared header
// directory, which is what makes the split easy to miss. Both need a runpath
// as well as a search path, since neither is on Solaris' default 64-bit search
// path; `scripts/build-sparc.sh` passes those through SPARC_LDFLAGS.
fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os != "solaris" {
        println!("cargo:warning=platform shims skipped: target_os is {}", target_os);
        return;
    }

    let x11_inc = std::env::var("X11_INCLUDE_DIR")
        .unwrap_or_else(|_| "/usr/openwin/include".to_owned());
    let x11_lib = std::env::var("X11_LIB_DIR")
        .unwrap_or_else(|_| "/usr/openwin/lib/sparcv9".to_owned());
    let sfw_lib = std::env::var("X11_SFW_LIB_DIR")
        .unwrap_or_else(|_| "/usr/openwin/sfw/lib/sparcv9".to_owned());

    // One archive per shim, so a link error names the half it came from.
    for shim in &["capture_shim", "input_shim", "cursor_shim"] {
        println!("cargo:rerun-if-changed=src/{}.c", shim);
        cc::Build::new()
            .file(format!("src/{}.c", shim))
            .include(&x11_inc)
            .opt_level(2)
            .compile(shim);
    }
    println!("cargo:rerun-if-changed=src/capture_shim.h");

    println!("cargo:rustc-link-search=native={}", x11_lib);
    println!("cargo:rustc-link-search=native={}", sfw_lib);
    println!("cargo:rustc-link-lib=Xext");      // MIT-SHM, for capture
    println!("cargo:rustc-link-lib=Xdamage");   // change reporting
    println!("cargo:rustc-link-lib=Xtst");      // XTEST, for injection
    println!("cargo:rustc-link-lib=Xfixes");    // the pointer's real shape
    println!("cargo:rustc-link-lib=X11");
}
