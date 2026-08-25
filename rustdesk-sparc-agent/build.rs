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
    for shim in &["capture_shim", "input_shim", "cursor_shim", "clipboard_shim",
                  "compat_shim"] {
        println!("cargo:rerun-if-changed=src/{}.c", shim);
        cc::Build::new()
            .file(format!("src/{}.c", shim))
            .include(&x11_inc)
            .opt_level(2)
            .compile(shim);
    }
    println!("cargo:rerun-if-changed=src/capture_shim.h");

    // The portable C the shared modules call: convert.rs and png.rs hand their
    // inner loops to this. It comes from the PowerPC tree with the rest of
    // them, so there is one copy of the conversion and one place to fix it.
    let ppc = "../rustdesk-ppc-agent/src";
    println!("cargo:rerun-if-changed={}/convert_shim.c", ppc);
    cc::Build::new()
        .file(format!("{}/convert_shim.c", ppc))
        .opt_level(2)
        .compile("convert_shim");
    // png.rs deflates with zlib, which Solaris ships.
    println!("cargo:rustc-link-lib=z");

    // The C libraries built on the Blade by scripts/build-deps.sh. This is a
    // path over *there*: it does not exist on this machine, which is exactly
    // how the ssh compiler wrapper tells a remote path from a local one.
    let deps = std::env::var("SPARC_DEPS").unwrap_or_else(|_| "/home/dani/sparc-deps".to_owned());
    println!("cargo:rustc-link-search=native={}/lib", deps);

    // The TLS the console and the API server talk over. mbedTLS rather than
    // OpenSSL because Solaris 10's own OpenSSL is 0.9.8 and speaks no TLS a
    // current server will accept.
    println!("cargo:rerun-if-changed={}/tls_shim.c", ppc);
    cc::Build::new()
        .file(format!("{}/tls_shim.c", ppc))
        .include(format!("{}/include", deps))
        .opt_level(2)
        .compile("tls_shim");
    // In dependency order: mbedtls needs x509 needs crypto.
    println!("cargo:rustc-link-lib=mbedtls");
    println!("cargo:rustc-link-lib=mbedx509");
    println!("cargo:rustc-link-lib=mbedcrypto");
    // Clipboard payloads from a current client arrive zstd-compressed.
    println!("cargo:rustc-link-lib=zstd");

    // VP8. The shim is the PowerPC agent's, so what the encoder does is
    // comparable between the ports rather than merely similar.
    println!("cargo:rerun-if-changed={}/vpx_shim.c", ppc);
    cc::Build::new()
        .file(format!("{}/vpx_shim.c", ppc))
        .include(format!("{}/include", deps))
        .opt_level(2)
        .compile("vpx_shim");
    println!("cargo:rustc-link-lib=vpx");

    println!("cargo:rustc-link-search=native={}", x11_lib);
    println!("cargo:rustc-link-search=native={}", sfw_lib);
    println!("cargo:rustc-link-lib=Xext");      // MIT-SHM, for capture
    println!("cargo:rustc-link-lib=Xdamage");   // change reporting
    println!("cargo:rustc-link-lib=Xtst");      // XTEST, for injection
    println!("cargo:rustc-link-lib=Xfixes");    // the pointer's real shape
    println!("cargo:rustc-link-lib=X11");
}
