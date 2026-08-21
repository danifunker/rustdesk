// Compile the C shims the portable modules need, so the self-test can link and
// actually run on IRIX rather than only typecheck.
//
//   convert_shim.c  `rd_argb_to_i420_rows` and `rd_argb_to_png_rows`, the
//                   hand-written inner loops convert.rs and png.rs call.
//   tls_shim.c      the mbedTLS wrapper http.rs calls for HTTPS. It compiles
//                   unmodified for IRIX; only the library search path differs.
//
// png.rs also calls zlib's compress2/uncompress, which IRIX 6.5 ships itself.
fn main() {
    let ppc = "../../../../rustdesk/rustdesk-ppc-agent";
    let sgug = std::env::var("SGUG_LIB_DIR")
        .unwrap_or_else(|_| "/opt/sgug-staging/usr/sgug/lib32".to_owned());
    let sgug_inc = std::env::var("SGUG_INCLUDE_DIR")
        .unwrap_or_else(|_| "/opt/sgug-staging/usr/sgug/include".to_owned());

    println!("cargo:rerun-if-changed={}/src/convert_shim.c", ppc);
    cc::Build::new()
        .file(format!("{}/src/convert_shim.c", ppc))
        .opt_level(2)
        .compile("convert_shim");

    // VP8. libvpx is built and verified for IRIX; the shim is the same one the
    // PowerPC agent uses, so encode behaviour stays comparable between ports.
    println!("cargo:rerun-if-changed={}/src/vpx_shim.c", ppc);
    cc::Build::new()
        .file(format!("{}/src/vpx_shim.c", ppc))
        .include(&sgug_inc)
        .opt_level(2)
        .compile("vpx_shim");
    println!("cargo:rustc-link-lib=static=vpx");

    println!("cargo:rerun-if-changed={}/src/tls_shim.c", ppc);
    cc::Build::new()
        .file(format!("{}/src/tls_shim.c", ppc))
        .include(&sgug_inc)
        .opt_level(2)
        .compile("tls_shim");

    // The IRIX capture shim, and the X libraries it calls. Only on the target:
    // there is no Xsgi to link against on the build host.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("irix") {
        println!("cargo:rerun-if-changed=../../../src/capture_shim.c");
        cc::Build::new()
            .file("../../../src/capture_shim.c")
            .opt_level(2)
            .compile("capture_shim");
        // Input injection over XTEST, presenting the same C interface input.rs
        // already calls on the Mac.
        println!("cargo:rerun-if-changed=../../../src/input_shim.c");
        cc::Build::new()
            .file("../../../src/input_shim.c")
            .opt_level(2)
            .compile("input_shim");
        // No -lXtst: IRIX ships XTEST only as a static archive that LLD cannot
        // consume, so input_shim.c issues the protocol requests itself.
        println!("cargo:rustc-link-lib=Xext");
        println!("cargo:rustc-link-lib=X11");
    }

    println!("cargo:rustc-link-search=native={}", sgug);
    println!("cargo:rustc-link-lib=static=mbedtls");
    println!("cargo:rustc-link-lib=static=mbedx509");
    println!("cargo:rustc-link-lib=static=mbedcrypto");
    println!("cargo:rustc-link-lib=z");
}
