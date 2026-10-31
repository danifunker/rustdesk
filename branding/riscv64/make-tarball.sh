#!/usr/bin/env bash
# Fork-only: turn the riscv64 R-DeskWay deb into a generic tarball for riscv64 distributions that
# do not use dpkg. Same files as the deb, plus install.sh, which does what the deb's maintainer
# scripts do (the /usr/bin/rustdesk link, the systemd service) after checking that every shared
# library the binaries need is present.
#
#   make-tarball.sh R-DESKWAY-RISCV64.deb OUT-DIR
set -euo pipefail

deb=${1:?deb}; outdir=${2:?output dir}
version=$(dpkg-deb -f "$deb" Version)
name=r-deskway-$version-riscv64
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
root=$work/$name
mkdir -p "$root"
dpkg-deb -x "$deb" "$root/files"

cat > "$root/install.sh" <<'EOF'
#!/bin/sh
# Install (or with -u, remove) R-DeskWay from this tarball. As root. DESTDIR stages the files
# somewhere else and skips the service, for packaging or for a look before installing.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dest=${DESTDIR:-}

if [ "${1:-}" = "-u" ]; then
    [ -n "$dest" ] || { systemctl disable --now rustdesk 2>/dev/null || true; }
    rm -f "$dest/usr/lib/systemd/system/rustdesk.service" "$dest/usr/bin/rustdesk"
    ( cd "$here/files" && find . -type f -o -type l ) | while read -r f; do rm -f "$dest/${f#./}"; done
    rm -rf "$dest/usr/share/rustdesk" "$dest/usr/lib/rustdesk"
    [ -n "$dest" ] || systemctl daemon-reload 2>/dev/null || true
    echo "R-DeskWay removed (its settings in ~/.config/rustdesk and /root/.config/rustdesk are kept)"
    exit 0
fi

[ "$(uname -m)" = riscv64 ] || { echo "this is the riscv64 build; this machine is $(uname -m)" >&2; exit 1; }
# Every library the binaries need must resolve here, or nothing is installed.
missing=$(LD_LIBRARY_PATH="$here/files/usr/share/rustdesk/lib" ldd \
    "$here/files/usr/share/rustdesk/rustdesk" "$here/files/usr/share/rustdesk/lib/"*.so \
    "$here/files/usr/lib/rustdesk/"libdrmtap.so.0.* 2>/dev/null | awk '/not found/ {print $1}' | sort -u)
if [ -n "$missing" ]; then
    echo "missing shared libraries -- install your distribution's packages for:" >&2
    echo "$missing" | sed 's/^/  /' >&2
    exit 1
fi
if [ -z "$dest" ] && [ -x /usr/bin/rustdesk ] && [ ! -L /usr/bin/rustdesk ]; then
    echo "another RustDesk is installed at /usr/bin/rustdesk; remove it first" >&2
    exit 1
fi

[ -n "$dest" ] || { systemctl stop rustdesk 2>/dev/null || true; }
( cd "$here/files" && tar -cf - . ) | ( mkdir -p "$dest/" && cd "$dest/" && tar -xf - --no-same-owner )
mkdir -p "$dest/usr/bin" "$dest/usr/lib/systemd/system"
ln -sf /usr/share/rustdesk/rustdesk "$dest/usr/bin/rustdesk"
cp "$here/files/usr/share/rustdesk/files/systemd/rustdesk.service" "$dest/usr/lib/systemd/system/"
if [ -z "$dest" ]; then
    systemctl daemon-reload
    systemctl enable --now rustdesk
    echo "R-DeskWay installed and started. Open it from your applications menu."
else
    echo "R-DeskWay staged in $dest (service not enabled)"
fi
EOF
chmod 755 "$root/install.sh"

cat > "$root/README.txt" <<EOF
R-DeskWay $version for riscv64 -- generic tarball

RustDesk with the DRM/KMS capture backend: unattended remote access on Wayland,
with no consent dialog, login screen included. Unofficial fork of RustDesk:
https://github.com/danifunker/rustdesk

On Debian or Ubuntu, use the .deb instead. This tarball is the same build for
other riscv64 distributions. It was built on Ubuntu 24.04 and needs glibc 2.38
or newer, systemd, and the shared libraries it links (among them GTK 3,
libvpx.so.9, libaom.so.3, libyuv.so.0, libopus, libsodium.so.23, libdrm, EGL).

  sudo ./install.sh          check the libraries, install, start the service
  sudo ./install.sh -u       remove it (settings are kept)

install.sh stops before installing anything if a library is missing, and names
it. Files go to /usr/share/rustdesk, /usr/lib/rustdesk (libdrmtap),
/usr/bin/rustdesk and /usr/lib/systemd/system/rustdesk.service.
EOF

tar -C "$work" -czf "$outdir/$name.tar.gz" "$name"
echo "wrote $outdir/$name.tar.gz"
