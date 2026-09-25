#!/usr/bin/env bash
# Fork-only: package the headless riscv64 R-DeskWay as a deb, in the same layout as the Flutter
# debs (binary in /usr/share/rustdesk, /usr/bin/rustdesk linked by res/DEBIAN/postinst, the root
# service, libdrmtap in /usr/lib/rustdesk where the drm backend dlopens it by absolute path), and
# with the same identity: Package r-deskway, conflicting with and replacing rustdesk and the old
# rustdesk-unattended-wayland. No launcher or icon: there is no window to launch.
#
#   package-headless-deb.sh RUSTDESK-BINARY LIBDRMTAP-BUILD-DIR OUT-DIR
set -euo pipefail

bin=${1:?rustdesk binary}; drmdir=${2:?libdrmtap build dir}; outdir=${3:?output dir}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
GNU=riscv64-linux-gnu

version=$(grep -m1 '^version' "$repo/Cargo.toml" | sed -E 's/.*"([^"]+)".*/\1/')
out="$outdir/r-deskway-$version-riscv64.deb"

# The build must be what the name promises: a riscv64 drm build with the display wake.
file -b "$bin" | grep -q 'RISC-V' || { echo "$bin is not a RISC-V binary" >&2; exit 1; }
grep -qaF /usr/lib/rustdesk/libdrmtap.so.0 "$bin" || { echo "$bin is not a drm build" >&2; exit 1; }
grep -qaF enable-drm-display-wake "$bin" || { echo "$bin has no drm-wake" >&2; exit 1; }
so=$(find "$drmdir" -maxdepth 1 -type f -regextype posix-extended -regex '.*/libdrmtap\.so\.0\.[0-9]+\.[0-9]+')
[ "$(printf '%s\n' "$so" | grep -c .)" = 1 ] || { echo "want one libdrmtap.so.0.x.y in $drmdir" >&2; exit 1; }
file -b "$so" | grep -q 'RISC-V' || { echo "$so is not a RISC-V library" >&2; exit 1; }

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
root=$work/pkg
install -Dm755 "$bin" "$root/usr/share/rustdesk/rustdesk"
$GNU-strip "$root/usr/share/rustdesk/rustdesk"
install -Dm644 "$repo/res/rustdesk.service" "$root/usr/share/rustdesk/files/systemd/rustdesk.service"
printf '#!/bin/sh\n' > "$root/usr/share/rustdesk/files/polkit"; chmod 755 "$root/usr/share/rustdesk/files/polkit"
install -Dm644 "$so" "$root/usr/lib/rustdesk/$(basename "$so")"
ln -s "$(basename "$so")" "$root/usr/lib/rustdesk/libdrmtap.so.0"
install -Dm644 "$here/README.headless" "$root/usr/share/doc/r-deskway/README.headless"

# The libc floor is whatever the newest symbol version either object needs.
floor=$( { $GNU-objdump -T "$root/usr/share/rustdesk/rustdesk"; $GNU-objdump -T "$root/usr/lib/rustdesk/$(basename "$so")"; } |
    grep -o 'GLIBC_2\.[0-9]*' | sort -t. -k2 -n -u | tail -1 | sed 's/GLIBC_//')

mkdir -p "$root/DEBIAN"
cp -a "$repo/res/DEBIAN/"* "$root/DEBIAN/"
cat > "$root/DEBIAN/control" <<EOF
Package: r-deskway
Section: net
Priority: optional
Version: $version
Architecture: riscv64
Maintainer: Dani Sarfati <dani@funkervogt.com>
Homepage: https://github.com/danifunker/rustdesk
Conflicts: rustdesk, rustdesk-unattended-wayland
Replaces: rustdesk, rustdesk-unattended-wayland
Provides: rustdesk
Depends: libc6 (>= $floor), libgtk-3-0t64 | libgtk-3-0, libxcb-randr0, libxdo3, libxfixes3, libxcb-shape0, libxcb-xfixes0, libasound2t64 | libasound2, libpulse0, libsystemd0, curl, libva2, libva-drm2, libva-x11-2, libgstreamer-plugins-base1.0-0, gstreamer1.0-pipewire, libvpx9, libaom3, libopus0, libyuv0, libdrm2 (>= 2.4.101), libegl1, libgles2
Recommends: libayatana-appindicator3-1
Installed-Size: $(du -sk --exclude=DEBIAN "$root" | cut -f1)
Description: R-DeskWay remote desktop, unattended on Wayland (headless riscv64 build)
 RustDesk with the DRM/KMS capture backend: controllable without a consent
 dialog, login screen included. This riscv64 build has no settings window
 (there is no Flutter engine for riscv64 at the pinned version); configure it
 from the command line -- see /usr/share/doc/r-deskway/README.headless.
EOF
( cd "$root" && find . -type f -not -path './DEBIAN/*' -printf '%P\0' | xargs -0 md5sum > DEBIAN/md5sums )
dpkg-deb --build --root-owner-group "$root" "$out" >/dev/null

echo "wrote $out"
dpkg-deb -f "$out" Package Version Architecture Depends
echo "NEEDED by the binary:"
$GNU-readelf -d "$root/usr/share/rustdesk/rustdesk" | awk '/NEEDED/ {print "  " $5}'
