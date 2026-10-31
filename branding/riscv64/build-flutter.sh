#!/usr/bin/env bash
# Fork-only: cross-build the FULL R-DeskWay (Flutter UI) for riscv64 on x86_64 Ubuntu 24.04.
#
# Flutter publishes no riscv64 engine, but meta-flutter/flutter-engine does, for a few Flutter
# versions; 3.44.2 is one (engine 77e2e947). Upstream RustDesk already builds one target (Windows
# arm64) with Flutter 3.44, from the same sources plus .github/patches/
# apply_flutter_3.44_source_patches.sh and a 3.44 bridge artifact; this reuses both. So:
#
#   Flutter 3.44.2 (x86_64 host, `flutter config --enable-riscv64`)
#   + meta-flutter's riscv64 engine (libflutter_linux_gtk.so, and gen_snapshot that runs on
#     x86_64 and targets riscv64) put where the tool looks for linux-riscv64 artifacts
#   + librustdesk.so cross-built with the same environment as the headless build (cross-env.sh)
#   -> build.py's own deb path (--flutter --drm --skip-cargo) -> rebrand-deb.sh
#
#   branding/riscv64/build-flutter.sh [deps|sdk|cargo|flutter|all]     (default: all)
#
#   deps      build-headless.sh's deps and drmtap, plus clang/lld for the Flutter runner
#   sdk       Flutter $FLUTTER_VERSION into $FLUTTER_ROOT, and the riscv64 engine into its cache
#   cargo     librustdesk.so for riscv64 with flutter, drm, drm-wake, linux-pkg-config
#   flutter   the 3.44 source patches, then build.py (flutter build linux + deb), then the
#             R-DeskWay rebrand: r-deskway-<version>-riscv64.deb
#
# The Flutter bridge (flutter/lib/generated_bridge*.dart, src/bridge_generated*.rs) must be
# present first: bridge.yml's bridge-artifact-flutter-3.44, unpacked at the repository root.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"
. "$here/cross-env.sh"

FLUTTER_VERSION=3.44.2
# meta-flutter's riscv64 engine for that version's engine revision (bin/internal/engine.version).
ENGINE=77e2e94772b6eb43759e34ed1ad7da4674e19cab
ENGINE_TAG=linux-engine-sdk-release-riscv64-$ENGINE
ENGINE_URL=https://github.com/meta-flutter/flutter-engine/releases/download/$ENGINE_TAG/$ENGINE_TAG.tar.gz
FLUTTER_ROOT=${FLUTTER_ROOT:-/opt/flutter-$FLUTTER_VERSION}
step=${1:-all}

deps() {
    "$here/build-headless.sh" deps
    "$here/build-headless.sh" drmtap
    sudo apt-get install -y --no-install-recommends clang lld cmake ninja-build curl xz-utils \
        unzip git rpm
}

sdk() {
    if [ ! -x "$FLUTTER_ROOT/bin/flutter" ]; then
        sudo mkdir -p "$FLUTTER_ROOT" && sudo chown "$(id -u):$(id -g)" "$FLUTTER_ROOT"
        curl -fsSL "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_$FLUTTER_VERSION-stable.tar.xz" |
            tar -xJ -C "$FLUTTER_ROOT" --strip-components=1
    fi
    git config --global --add safe.directory '*'
    local fl="$FLUTTER_ROOT/bin/flutter"
    [ "$(cat "$FLUTTER_ROOT/bin/internal/engine.version")" = "$ENGINE" ] ||
        { echo "Flutter $FLUTTER_VERSION's engine is not $ENGINE" >&2; exit 1; }
    "$fl" config --no-analytics >/dev/null
    "$fl" config --enable-riscv64
    "$fl" precache --linux

    # The riscv64 engine. The tool only ever downloads the HOST's linux-<arch> artifacts, so
    # these directories are ours and are never refreshed over.
    local cache="$FLUTTER_ROOT/bin/cache/artifacts/engine" work
    work=$(mktemp -d)
    curl -fsSL -o "$work/e.tar.gz" "$ENGINE_URL"
    curl -fsSL -o "$work/e.sha256" "$ENGINE_URL.sha256"
    ( cd "$work" && echo "$(awk '{print $1}' e.sha256)  e.tar.gz" | sha256sum -c - )
    tar -xzf "$work/e.tar.gz" -C "$work" --wildcards \
        '*/engine-sdk/lib/libflutter_linux_gtk.so' '*/engine-sdk/clang_x64/bin/gen_snapshot' \
        '*/engine-sdk/clang_x64/lib64/*'
    local sdkdir; sdkdir=$(dirname "$(find "$work" -type d -name clang_x64 | head -1)")
    for d in linux-riscv64 linux-riscv64-release; do
        rm -rf "${cache:?}/$d"; mkdir -p "$cache/$d"
        cp "$sdkdir/lib/libflutter_linux_gtk.so" "$cache/$d/"
        # gen_snapshot runs on the x86_64 host; keep the libc it was linked against beside it.
        cp -r "$sdkdir/clang_x64/lib64" "$cache/$d/lib64"
        cp "$sdkdir/clang_x64/bin/gen_snapshot" "$cache/$d/gen_snapshot"
        cp "$cache/$d/gen_snapshot" "$cache/$d/gen_snapshot_riscv64"
        # Architecture-independent: the embedder headers and ICU data from the x64 release.
        cp -r "$cache/linux-x64-release/flutter_linux" "$cache/$d/"
        cp "$cache/linux-x64/icudtl.dat" "$cache/$d/"
    done
    rm -rf "$work"
    file "$cache/linux-riscv64-release/libflutter_linux_gtk.so" "$cache/linux-riscv64-release/gen_snapshot"
    "$cache/linux-riscv64-release/gen_snapshot" --version 2>&1 | head -1 ||
        { echo "the riscv64 gen_snapshot does not run on this host" >&2; exit 1; }
}

cargo_build() {
    [ -f src/bridge_generated.rs ] ||
        { echo "no Flutter bridge: unpack bridge-artifact-flutter-3.44 here first" >&2; exit 1; }
    # Only the cdylib is needed: it is what the Flutter runner loads.
    sed -i 's/\["cdylib", "staticlib", "rlib"\]/["cdylib"]/' Cargo.toml
    riscv64_cargo_env
    riscv64_mozjpeg_fallback "$repo"
    cargo build --locked --release --keep-going --target "$RV_TARGET" --lib \
        --features flutter,drm,drm-wake,linux-pkg-config,unix-file-copy-paste
    # The runner's CMakeLists links ../../target/release/liblibrustdesk.so.
    mkdir -p target/release
    cp "target/$RV_TARGET/release/liblibrustdesk.so" target/release/liblibrustdesk.so
    file target/release/liblibrustdesk.so
}

flutter_build() {
    bash .github/patches/apply_flutter_3.44_source_patches.sh
    export PATH="$FLUTTER_ROOT/bin:$PATH"
    # build.py's flutter path, retargeted the way the aarch64 job retargets it: cross-build for
    # riscv64 against the multiarch libraries in /, and package the riscv64 bundle.
    sed -i 's|flutter build linux --release|flutter build linux --release --target-platform linux-riscv64 --target-sysroot /|' build.py
    sed -i 's|x64/release|riscv64/release|g' build.py
    # The runner's CMake finds GTK and friends with pkg-config: the riscv64 .pc files, not the host's.
    export PKG_CONFIG_LIBDIR=$RV_PCDIR:/usr/share/pkgconfig
    export DEB_ARCH=riscv64
    export DRMTAP_PREBUILT_DIR="$repo/third_party/libdrmtap/build-riscv64"
    export CARGO_INCREMENTAL=0
    python3 build.py --flutter --drm --unix-file-copy-paste --skip-cargo
    local ins=(rustdesk-unattended-wayland-*.deb)
    [ "${#ins[@]}" = 1 ] || { echo "expected one stock drm deb, found: ${ins[*]}" >&2; exit 1; }
    local version; version=$(grep -m1 '^version' Cargo.toml | sed -E 's/.*"([^"]+)".*/\1/')
    bash branding/rebrand-deb.sh "${ins[0]}" riscv64 "r-deskway-$version-riscv64.deb"
    rm -f "${ins[0]}"
    add_depends "r-deskway-$version-riscv64.deb"
}

# build.py's control file lists what the vcpkg builds need; this one links the distro's codec and
# crypto libraries instead (linux-pkg-config, libsodium through pkg-config), so name them. And the
# tray needs libayatana-appindicator3-1: board images that skip Recommends never install it.
add_depends() {
    local deb=$1 work; work=$(mktemp -d)
    dpkg-deb -R "$deb" "$work/pkg"
    sed -i 's/^Depends: .*/&, libvpx9, libaom3, libopus0, libyuv0, libsodium23, libayatana-appindicator3-1/' \
        "$work/pkg/DEBIAN/control"
    sed -i '/^Recommends: libayatana-appindicator3-1$/d' "$work/pkg/DEBIAN/control"
    dpkg-deb --build --root-owner-group "$work/pkg" "$deb" >/dev/null
    rm -rf "$work"
    dpkg-deb -f "$deb" Depends
}

case "$step" in
deps) deps ;;
sdk) sdk ;;
cargo) cargo_build ;;
flutter) flutter_build ;;
depends) add_depends "${2:?deb}" ;;
all) deps; sdk; cargo_build; flutter_build ;;
*) echo "usage: $0 [deps|sdk|cargo|flutter|all]" >&2; exit 1 ;;
esac
