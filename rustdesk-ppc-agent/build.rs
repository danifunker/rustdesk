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

/// An environment variable, preferring the target-specific spelling.
///
/// `MBEDTLS_INCLUDE_DIR_powerpc_apple_darwin` beats `MBEDTLS_INCLUDE_DIR`, the
/// same convention `CC_powerpc_apple_darwin` follows here and for the same
/// reason: **one release run builds for the host and for the Mac**, and the two
/// do not share a filesystem. Exporting only the plain name for a cross build
/// points the *host* build at a directory that exists only on the G5, and
/// cc-rs then fails on a `-I` it cannot open -- which is exactly what happened
/// the first time `build-release.sh` was run with this.
fn target_var(name: &str) -> Option<String> {
    let target = std::env::var("TARGET").unwrap_or_default().replace('-', "_");
    let specific = format!("{}_{}", name, target);
    println!("cargo:rerun-if-env-changed={}", specific);
    println!("cargo:rerun-if-env-changed={}", name);
    std::env::var(&specific).ok().or_else(|| std::env::var(name).ok())
}

/// Where mbedTLS is, as (include dir, lib dir).
///
/// Searched rather than assumed, because this build has three homes: MacPorts
/// (`${prefix}`), the cross build's `~/ppc-libs`, and a host checkout testing
/// the console client before it reaches a G5. The explicit variables win, so
/// none of the guessing below can surprise a deliberate build.
fn mbedtls_paths() -> Option<(std::path::PathBuf, std::path::PathBuf)> {
    use std::path::PathBuf;
    let has_header = |inc: &PathBuf| inc.join("mbedtls").join("ssl.h").is_file();

    if let (Some(inc), Some(lib)) =
        (target_var("MBEDTLS_INCLUDE_DIR"), target_var("MBEDTLS_LIB_DIR"))
    {
        // Not checked for existence on purpose: for the PowerPC target these
        // name directories on the *Mac*, which this machine cannot see. The
        // remote cc wrapper passes such a path through untouched -- that is the
        // same mechanism `-L$PPC_LIBS_DIR` already relies on.
        return Some((PathBuf::from(inc), PathBuf::from(lib)));
    }
    let mut prefixes: Vec<PathBuf> = Vec::new();
    if let Some(d) = target_var("MBEDTLS_DIR") {
        prefixes.push(PathBuf::from(d));
    }
    println!("cargo:rerun-if-env-changed=PPC_LIBS_DIR");
    // The cross build puts the target's libraries here; its headers sit beside
    // them under the same prefix.
    if let Ok(d) = std::env::var("PPC_LIBS_DIR") {
        if let Some(parent) = PathBuf::from(d).parent() {
            prefixes.push(parent.to_path_buf());
        }
    }
    // MacPorts first: on a Mac that has both, the port is the one this is
    // built and tested against.
    prefixes.push(PathBuf::from("/opt/local"));
    prefixes.push(PathBuf::from("/usr/local"));
    prefixes.push(PathBuf::from("/usr"));

    for p in prefixes {
        let inc = p.join("include");
        if has_header(&inc) {
            // `library/` is where an in-tree mbedTLS build leaves its archives;
            // `lib/` is where an installed one does.
            for name in ["lib", "library"] {
                let lib = p.join(name);
                if lib.is_dir() {
                    return Some((inc, lib));
                }
            }
        }
    }
    None
}

/// Build the TLS shim if mbedTLS can be found, and say plainly when it cannot.
///
/// Not gated on macOS: the console client is ordinary sockets, and being able
/// to exercise it on the host is what keeps a TLS bug off the G5. When mbedTLS
/// is absent the crate still builds and `https` fails at runtime with a message
/// saying so, which is better than a build that cannot be produced at all on a
/// machine that only ever needed the LAN path.
fn build_tls() {
    match mbedtls_paths() {
        Some((inc, lib)) => {
            cc::Build::new()
                .file("src/tls_shim.c")
                .flag(NO_MISCOMPILE)
                .include(&inc)
                .opt_level(2)
                .compile("tlsshim");
            println!("cargo:rustc-link-search=native={}", lib.display());
            // Static by default, because the .app in deploy/ is installed on
            // Macs that have never heard of MacPorts. `MBEDTLS_STATIC=0` is for
            // a host build that only has shared objects.
            let kind = if std::env::var("MBEDTLS_STATIC").as_deref() == Ok("0") {
                ""
            } else {
                "static="
            };
            // Order matters to a static link: ssl needs x509 needs crypto.
            for l in ["mbedtls", "mbedx509", "mbedcrypto"] {
                println!("cargo:rustc-link-lib={}{}", kind, l);
            }
            println!("cargo:rerun-if-changed=src/tls_shim.c");
        }
        None => {
            println!("cargo:rustc-cfg=no_tls");
            println!(
                "cargo:warning=mbedTLS not found, so https is not built in and an \
                 https console will be refused at runtime. Set MBEDTLS_DIR, or \
                 install the mbedtls3 port."
            );
        }
    }
}

fn main() {
    build_tls();

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
