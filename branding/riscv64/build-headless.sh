#!/usr/bin/env bash
# Fork-only: cross-build R-DeskWay for riscv64 on an x86_64 Ubuntu 24.04 host, HEADLESS.
#
# There is no Flutter engine for riscv64 at the Flutter version this tree pins (3.24.5), so this
# builds the non-Flutter binary: the root service (DRM/KMS capture through libdrmtap), the --server
# process, the ID/relay client and input -- everything unattended access needs -- with no settings
# window. It is configured from the command line (see package-headless-deb.sh's README).
#
#   branding/riscv64/build-headless.sh [deps|drmtap|cargo|package|all]     (default: all)
#
#   deps      multiarch: riscv64 packages from ports.ubuntu.com beside the host's amd64 ones, the
#             riscv64 cross gcc, and the -dev packages the build links (needs sudo)
#   drmtap    libdrmtap at the pin in build.py, cross-built with meson, into
#             third_party/libdrmtap/build-riscv64
#   cargo     `rustdesk` for riscv64gc-unknown-linux-gnu with drm, drm-wake and
#             linux-pkg-config (the distro's libvpx, libaom, libyuv and libopus, not vcpkg)
#   package   package-headless-deb.sh: r-deskway-<version>-riscv64.deb
#
# Ubuntu 24.04 because that is what riscv64 boards ship (the Orange Pi RV2 this was tested on runs
# it), so the libraries linked here are the ones the package depends on there.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"

T=riscv64gc-unknown-linux-gnu
GNU=riscv64-linux-gnu
PCDIR=/usr/lib/$GNU/pkgconfig
step=${1:-all}

deps() {
    # Keep the host's own sources amd64-only, then add the riscv64 ports archive at the same
    # pockets, so every :riscv64 -dev package is the same version as its amd64 twin.
    local s=/etc/apt/sources.list.d/ubuntu.sources
    if [ -f "$s" ] && ! grep -q '^Architectures:' "$s"; then
        sudo sed -i 's/^Types: deb$/Types: deb\nArchitectures: amd64/' "$s"
    fi
    sudo tee /etc/apt/sources.list.d/ports-riscv64.sources >/dev/null <<'EOF'
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: noble noble-updates noble-security
Components: main restricted universe multiverse
Architectures: riscv64
EOF
    sudo dpkg --add-architecture riscv64
    sudo apt-get update -y
    sudo apt-get install -y --no-install-recommends \
        gcc-$GNU g++-$GNU binutils-$GNU clang libclang-dev pkg-config meson ninja-build \
        cmake python3 file dpkg-dev libsodium-dev
    sudo apt-get install -y --no-install-recommends \
        libc6-dev:riscv64 libgtk-3-dev:riscv64 libxcb-randr0-dev:riscv64 \
        libxcb-shape0-dev:riscv64 libxcb-xfixes0-dev:riscv64 libxdo-dev:riscv64 \
        libxfixes-dev:riscv64 libasound2-dev:riscv64 libpulse-dev:riscv64 libva-dev:riscv64 \
        libgstreamer1.0-dev:riscv64 libgstreamer-plugins-base1.0-dev:riscv64 \
        libayatana-appindicator3-dev:riscv64 libpam0g-dev:riscv64 libssl-dev:riscv64 \
        libvpx-dev:riscv64 libaom-dev:riscv64 libopus-dev:riscv64 libyuv-dev:riscv64 \
        libdrm-dev:riscv64 libegl-dev:riscv64 libgles-dev:riscv64 \
        libseccomp-dev:riscv64 libcap-dev:riscv64 libsodium-dev:riscv64
    # Ubuntu's libyuv-dev ships no .pc, and scrap's linux-pkg-config path probes for one.
    sudo tee "$PCDIR/libyuv.pc" >/dev/null <<EOF
prefix=/usr
libdir=\${prefix}/lib/$GNU
includedir=\${prefix}/include

Name: libyuv
Description: YUV conversion and scaling (pkg-config file written by R-DeskWay's riscv64 build)
Version: 0.0
Libs: -L\${libdir} -lyuv
Cflags: -I\${includedir}
EOF
}

drmtap() {
    # The pin lives in build.py only; read it from there rather than repeating it.
    local repo_url sha
    read -r repo_url sha < <(python3 - <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("b", "build.py")
b = importlib.util.module_from_spec(spec)
sys.argv = ["build.py"]
spec.loader.exec_module(b)
print(b.LIBDRMTAP_REPO_PINNED, b.LIBDRMTAP_SHA_PINNED)
PY
)
    local src=third_party/libdrmtap
    if [ ! -f "$src/meson.build" ]; then
        rm -rf "$src"; mkdir -p "$src"
        git -C "$src" init -q
        git -C "$src" remote add origin "$repo_url"
        git -C "$src" fetch -q --depth 1 origin "$sha"
        git -C "$src" checkout -q FETCH_HEAD
    fi
    [ "$(git -C "$src" rev-parse HEAD)" = "$sha" ] || { echo "libdrmtap is not at the pin $sha" >&2; exit 1; }
    cat > "$src/riscv64.cross" <<EOF
[binaries]
c = '$GNU-gcc'
cpp = '$GNU-g++'
ar = '$GNU-ar'
strip = '$GNU-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$PCDIR:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'riscv64'
cpu = 'riscv64'
endian = 'little'
EOF
    rm -rf "$src/build-riscv64"
    meson setup "$src/build-riscv64" "$src" --buildtype=release --cross-file "$src/riscv64.cross"
    meson compile -C "$src/build-riscv64" drmtap:shared_library
    file "$src"/build-riscv64/libdrmtap.so.0.*
}

cargo_build() {
    # Only the rlib is needed under the binary; the cdylib/staticlib are the Flutter bridge's.
    sed -i 's/\["cdylib", "staticlib", "rlib"\]/["rlib"]/' Cargo.toml
    export CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER=$GNU-gcc
    export CC_riscv64gc_unknown_linux_gnu=$GNU-gcc
    export CXX_riscv64gc_unknown_linux_gnu=$GNU-g++
    export AR_riscv64gc_unknown_linux_gnu=$GNU-ar
    export PKG_CONFIG_ALLOW_CROSS=1
    export PKG_CONFIG_LIBDIR_riscv64gc_unknown_linux_gnu=$PCDIR:/usr/share/pkgconfig
    export BINDGEN_EXTRA_CLANG_ARGS_riscv64gc_unknown_linux_gnu="--target=$GNU -I/usr/include/$GNU"
    # libsodium-sys would run libsodium's configure with --host=riscv64gc-unknown-linux-gnu, which
    # autoconf does not know. Use pkg-config instead: its target-scoped LIBDIR above finds the
    # riscv64 libsodium for the target, while build scripts (built for the x86_64 host, and
    # rustdesk's pulls in hbb_common) find the host's. SODIUM_LIB_DIR cannot be scoped like that.
    export SODIUM_USE_PKG_CONFIG=1
    # mozjpeg-sys 2.2.2 (camera support, via nokhwa-core) enables SIMD on any arch with a GNU
    # assembler but has no riscv64 SIMD code, and then leaves out its jsimd_none.c fallback, so
    # the jsimd_* hooks are undefined at the final link. Build the crate once so its generated
    # headers exist, compile its own jsimd_none.c against them, and link that object in.
    cargo build --locked --release --target "$T" -p mozjpeg-sys
    local mzv mzsrc mzout
    mzv=$(grep -A1 '^name = "mozjpeg-sys"$' Cargo.lock | sed -n 's/^version = "\(.*\)"$/\1/p')
    mzsrc=$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/mozjpeg-sys-"$mzv" | head -1)
    mzout=$(ls -d target/"$T"/release/build/mozjpeg-sys-*/out | head -1)
    $GNU-gcc -O2 -fPIC -c "$mzsrc/vendor/jsimd_none.c" -I"$mzout/include" -I"$mzsrc/vendor" \
        -o target/jsimd_none-riscv64.o
    export CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_RUSTFLAGS="-C link-arg=$repo/target/jsimd_none-riscv64.o"
    cargo build --locked --release --keep-going --target "$T" --bin rustdesk \
        --features drm,drm-wake,linux-pkg-config
    file "target/$T/release/rustdesk"
}

package() {
    "$here/package-headless-deb.sh" \
        "target/$T/release/rustdesk" third_party/libdrmtap/build-riscv64 .
}

case "$step" in
deps) deps ;;
drmtap) drmtap ;;
cargo) cargo_build ;;
package) package ;;
all) deps; drmtap; cargo_build; package ;;
*) echo "usage: $0 [deps|drmtap|cargo|package|all]" >&2; exit 1 ;;
esac
