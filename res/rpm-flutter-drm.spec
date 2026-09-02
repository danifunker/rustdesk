# DRM / unattended-wayland variant of rpm-flutter.spec. Fork-only.
#
# Same package as rpm-flutter.spec but carrying the DRM/KMS consent-free capture backend: it bundles
# libdrmtap.so under /usr/lib/rustdesk and is named DISTINCTLY (rustdesk-unattended-wayland), which is
# the informed consent this feature rests on -- exactly as the deb variant does in build.py. It
# Conflicts/Obsoletes/Provides the stock `rustdesk` so you install one or the other, never both.
#
# The compiled flutter bundle and the built libdrmtap .so are produced by the same CI build that makes
# the deb; this spec only repackages them, so %build has nothing to do. $HBB is the repo root and
# points rpmbuild at both. Keep in step with res/rpm-flutter.spec when syncing from upstream.
Name:       rustdesk-unattended-wayland
Version:    1.5.0
Release:    0
Summary:    RustDesk with unattended Wayland (DRM/KMS) capture
License:    GPL-3.0
URL:        https://rustdesk.com
Vendor:     rustdesk <info@rustdesk.com>
Requires:   gtk3 libxcb libXfixes alsa-lib libva gstreamer1-plugins-base libdrm mesa-libEGL mesa-libGLES
Recommends: libayatana-appindicator-gtk3 libxdo
Conflicts:  rustdesk
Obsoletes:  rustdesk
Provides:   rustdesk
Provides:   libdesktop_drop_plugin.so()(64bit), libdesktop_multi_window_plugin.so()(64bit), libfile_selector_linux_plugin.so()(64bit), libflutter_custom_cursor_plugin.so()(64bit), libflutter_linux_gtk.so()(64bit), libscreen_retriever_plugin.so()(64bit), libtray_manager_plugin.so()(64bit), liburl_launcher_linux_plugin.so()(64bit), libwindow_manager_plugin.so()(64bit), libwindow_size_plugin.so()(64bit), libtexture_rgba_renderer_plugin.so()(64bit)

# https://docs.fedoraproject.org/en-US/packaging-guidelines/Scriptlets/

%description
The best open-source remote desktop client software, written in Rust.
This build captures the Wayland screen directly from the kernel DRM/KMS
scanout (no xdg-desktop-portal consent dialog), so it can serve an
unattended session and the login screen. It installs libdrmtap privately
under /usr/lib/rustdesk and conflicts with the stock rustdesk package.

%prep
# we have no source, so nothing here

%build
# we have no source, so nothing here

%install

mkdir -p "%{buildroot}/usr/share/rustdesk" && cp -r ${HBB}/flutter/build/linux/x64/release/bundle/* -t "%{buildroot}/usr/share/rustdesk"
mkdir -p "%{buildroot}/usr/bin"
install -Dm 644 $HBB/res/rustdesk.service -t "%{buildroot}/usr/share/rustdesk/files"
install -Dm 644 $HBB/res/rustdesk.desktop -t "%{buildroot}/usr/share/rustdesk/files"
install -Dm 644 $HBB/res/rustdesk-link.desktop -t "%{buildroot}/usr/share/rustdesk/files"
install -Dm 644 $HBB/res/128x128@2x.png "%{buildroot}/usr/share/icons/hicolor/256x256/apps/rustdesk.png"
install -Dm 644 $HBB/res/scalable.svg "%{buildroot}/usr/share/icons/hicolor/scalable/apps/rustdesk.svg"

# --- DRM backend: stage libdrmtap under /usr/lib/rustdesk (resolved by absolute path at dlopen) ---
# The one real versioned object (libdrmtap.so.0.X.Y), never the .so/.so.0 symlinks meson also leaves.
_drmdir="${HBB}/third_party/libdrmtap/build-pkg"
_so="$(find "${_drmdir}" -maxdepth 1 -type f -regex '.*/libdrmtap\.so\.0\.[0-9]+\.[0-9]+' | head -1)"
if [ -z "${_so}" ]; then echo "rpm-flutter-drm: no versioned libdrmtap.so.0.x.y in ${_drmdir}" >&2; exit 1; fi
mkdir -p "%{buildroot}/usr/lib/rustdesk"
install -Dm 755 "${_so}" "%{buildroot}/usr/lib/rustdesk/$(basename "${_so}")"
ln -sf "$(basename "${_so}")" "%{buildroot}/usr/lib/rustdesk/libdrmtap.so.0"

%files
/usr/share/rustdesk/*
/usr/share/rustdesk/files/rustdesk.service
/usr/lib/rustdesk/*
/usr/share/icons/hicolor/256x256/apps/rustdesk.png
/usr/share/icons/hicolor/scalable/apps/rustdesk.svg
/usr/share/rustdesk/files/rustdesk.desktop
/usr/share/rustdesk/files/rustdesk-link.desktop

%changelog
# let's skip this for now

%pre
case "$1" in
  1)
    # for install
  ;;
  2)
    # for upgrade
    systemctl stop rustdesk || true
  ;;
esac

%post
cp /usr/share/rustdesk/files/rustdesk.service /etc/systemd/system/rustdesk.service
cp /usr/share/rustdesk/files/rustdesk.desktop /usr/share/applications/
cp /usr/share/rustdesk/files/rustdesk-link.desktop /usr/share/applications/
ln -sf /usr/share/rustdesk/rustdesk /usr/bin/rustdesk
systemctl daemon-reload
systemctl enable rustdesk
systemctl start rustdesk
update-desktop-database

%preun
case "$1" in
  0)
    # for uninstall
    systemctl stop rustdesk || true
    systemctl disable rustdesk || true
    rm /etc/systemd/system/rustdesk.service || true
  ;;
  1)
    # for upgrade
  ;;
esac

%postun
case "$1" in
  0)
    # for uninstall
    rm /usr/bin/rustdesk || true
    rm -f /usr/lib/rustdesk/libdrmtap.so.0 || true
    rmdir /usr/lib/rustdesk || true
    rmdir /usr/local/rustdesk || true
    rmdir /usr/share/rustdesk || true
    rm /usr/share/applications/rustdesk.desktop || true
    rm /usr/share/applications/rustdesk-link.desktop || true
    update-desktop-database
  ;;
  1)
    # for upgrade
    rmdir /usr/lib/rustdesk || true
    rmdir /usr/local/rustdesk || true
  ;;
esac
