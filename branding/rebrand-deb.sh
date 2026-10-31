#!/usr/bin/env bash
# Fork-only: turn the stock-built rustdesk-unattended-wayland deb into the rebranded R-DeskWay deb,
# WITHOUT touching build.py or any upstream file. Renames the package, sets the fork maintainer and
# homepage, swaps in the rusty icon + R-DeskWay launcher, fixes md5sums/Installed-Size, repacks.
# The binary, service and config dir stay `rustdesk` on purpose: it remains a drop-in that preserves
# settings and conflicts/replaces the stock package, and nothing upstream is modified (rebase-safe).
#
# Usage: rebrand-deb.sh <input.deb> <deb_arch> <output.deb>
set -euo pipefail

in_deb="${1:?input deb}"; deb_arch="${2:?deb arch}"; out_deb="${3:?output deb}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # the branding/ dir holds the assets

PKG_NAME="r-deskway"
MAINTAINER="Dani Sarfati <dani@funkervogt.com>"
HOMEPAGE="https://github.com/danifunker/rustdesk"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
root="$work/pkg"
dpkg-deb -R "$in_deb" "$root"

ctrl="$root/DEBIAN/control"
# Rename the package and re-owner it. Keep the Conflicts/Replaces/Provides: rustdesk that build.py
# already added, and additionally supersede the old rustdesk-unattended-wayland name.
sed -i \
  -e "s/^Package: .*/Package: ${PKG_NAME}/" \
  -e "s|^Maintainer: .*|Maintainer: ${MAINTAINER}|" \
  -e "s|^Homepage: .*|Homepage: ${HOMEPAGE}|" \
  "$ctrl"
if grep -q '^Conflicts:' "$ctrl"; then
  grep -q 'rustdesk-unattended-wayland' "$ctrl" || \
    sed -i "s/^Conflicts: .*/&, rustdesk-unattended-wayland/" "$ctrl"
fi
grep -q '^Replaces:' "$ctrl" && sed -i "s/^Replaces: .*/&, rustdesk-unattended-wayland/" "$ctrl" || true

# Rusty icon + R-DeskWay launcher (installed under the same filenames so Icon=rustdesk resolves and
# the window class is unchanged).
install -Dm644 "$here/r-deskway-256.png" "$root/usr/share/icons/hicolor/256x256/apps/rustdesk.png"
install -Dm644 "$here/r-deskway.svg"     "$root/usr/share/icons/hicolor/scalable/apps/rustdesk.svg"
install -Dm644 "$here/r-deskway.desktop" "$root/usr/share/applications/rustdesk.desktop"

# Regenerate md5sums for the (now changed) data files, and refresh Installed-Size.
( cd "$root" && find . -type f -not -path './DEBIAN/*' -printf '%P\0' | xargs -0 md5sum > DEBIAN/md5sums )
size_kib="$(du -sk --exclude=DEBIAN "$root" | cut -f1)"
if grep -q '^Installed-Size:' "$ctrl"; then
  sed -i "s/^Installed-Size: .*/Installed-Size: ${size_kib}/" "$ctrl"
fi

# Assert the rename really took, then build.
grep -qx "Package: ${PKG_NAME}" "$ctrl" || { echo "rebrand-deb: Package rename failed" >&2; exit 1; }
dpkg-deb --build --root-owner-group "$root" "$out_deb" >/dev/null
echo "rebrand-deb: wrote $out_deb"
dpkg-deb -f "$out_deb" Package Maintainer Architecture Conflicts Replaces Provides
