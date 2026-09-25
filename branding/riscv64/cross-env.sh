# Fork-only: the environment for cross-compiling rustdesk for riscv64 on x86_64 Ubuntu 24.04.
# Sourced by build-headless.sh and build-flutter.sh so the two builds cannot drift apart.
#
#   . branding/riscv64/cross-env.sh; riscv64_cargo_env
#
# Expects the multiarch packages build-headless.sh's `deps` step installs.

RV_TARGET=riscv64gc-unknown-linux-gnu
RV_GNU=riscv64-linux-gnu
RV_PCDIR=/usr/lib/$RV_GNU/pkgconfig

riscv64_cargo_env() {
    export CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER=$RV_GNU-gcc
    export CC_riscv64gc_unknown_linux_gnu=$RV_GNU-gcc
    export CXX_riscv64gc_unknown_linux_gnu=$RV_GNU-g++
    export AR_riscv64gc_unknown_linux_gnu=$RV_GNU-ar
    export PKG_CONFIG_ALLOW_CROSS=1
    export PKG_CONFIG_LIBDIR_riscv64gc_unknown_linux_gnu=$RV_PCDIR:/usr/share/pkgconfig
    export BINDGEN_EXTRA_CLANG_ARGS_riscv64gc_unknown_linux_gnu="--target=$RV_GNU -I/usr/include/$RV_GNU"
    # libsodium-sys would run libsodium's configure with --host=riscv64gc-unknown-linux-gnu, which
    # autoconf does not know. Use pkg-config instead: its target-scoped LIBDIR above finds the
    # riscv64 libsodium for the target, while build scripts (built for the x86_64 host, and
    # rustdesk's pulls in hbb_common) find the host's. SODIUM_LIB_DIR cannot be scoped like that.
    export SODIUM_USE_PKG_CONFIG=1
}

# mozjpeg-sys 2.2.2 (camera support, via nokhwa-core) enables SIMD on any arch with a GNU
# assembler but has no riscv64 SIMD code, and then leaves out its jsimd_none.c fallback, so the
# jsimd_* hooks are undefined at the final link. Build the crate once so its generated headers
# exist, compile its own jsimd_none.c against them, and link that object in.
riscv64_mozjpeg_fallback() {
    local repo=$1 mzv mzsrc mzout
    cargo build --locked --release --target "$RV_TARGET" -p mozjpeg-sys
    mzv=$(grep -A1 '^name = "mozjpeg-sys"$' Cargo.lock | sed -n 's/^version = "\(.*\)"$/\1/p')
    mzsrc=$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/mozjpeg-sys-"$mzv" | head -1)
    mzout=$(ls -d target/"$RV_TARGET"/release/build/mozjpeg-sys-*/out | head -1)
    $RV_GNU-gcc -O2 -fPIC -c "$mzsrc/vendor/jsimd_none.c" -I"$mzout/include" -I"$mzsrc/vendor" \
        -o target/jsimd_none-riscv64.o
    export CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_RUSTFLAGS="-C link-arg=$repo/target/jsimd_none-riscv64.o"
}
