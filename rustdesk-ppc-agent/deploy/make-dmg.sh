#!/usr/bin/env bash
#
# Build a drag-to-Applications disk image from a bundled .app.
#
#   ./deploy/make-dmg.sh target/rustdesk-agent-universal.tar.gz \
#                        target/release/0.1.0/Agent-for-RustDesk-PPC-0.1.0-universal.dmg \
#                        "Agent for RustDesk PPC"
#
# The input is a tarball from deploy/bundle.sh; the output is a compressed
# read-only image that opens on a window with the app on the left, an
# Applications alias on the right and an arrow between them.
#
# It runs on the Mac, because `hdiutil` is Darwin's and the layout is set by
# talking to the Finder. Both work over ssh -- the Finder accepts window and
# icon geometry without a GUI login, because setting properties is not "user
# interaction" in the sense that made dialogs fail with -1713. Measured, not
# assumed.
#
# THE LAYOUT NUMBERS ARE DUPLICATED in deploy/make-dmg-bg.py, which draws the
# background at exactly this size with the arrow between exactly these two icon
# positions. The Finder scales nothing, so a mismatch is not an error -- it is
# an arrow pointing at empty space.
set -euo pipefail

TARBALL="${1:?usage: make-dmg.sh <bundle.tar.gz> <output.dmg> <volume name>}"
OUTDMG="${2:?}"
VOLNAME="${3:-Agent for RustDesk PPC}"
HOST="${PPC_HOST:-ppctiger}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export SSH_AUTH_SOCK="${SSH_AUTH_SOCK:-/tmp/ssh-agent-ppc.sock}"

[ -f "$TARBALL" ] || { echo "error: no bundle at $TARBALL" >&2; exit 1; }
[ -f "$HERE/deploy/dmg-background.png" ] || {
    echo "error: no deploy/dmg-background.png -- run ./deploy/make-dmg-bg.py" >&2; exit 1; }

REMOTE="/tmp/rd-dmg-$$"
ssh "$HOST" "mkdir -p '$REMOTE'"
scp -q "$TARBALL" "$HOST:$REMOTE/bundle.tar.gz"
scp -q "$HERE/deploy/dmg-background.png" "$HOST:$REMOTE/bg.png"

ssh "$HOST" "REMOTE='$REMOTE' VOLNAME='$VOLNAME' bash -s" <<'REMOTE_SH'
set -euo pipefail
cd "$REMOTE"

WIN_W=600; WIN_H=400          # keep in step with make-dmg-bg.py
ICON_Y=200; APP_X=150; DEST_X=450

mkdir -p stage
tar xzf bundle.tar.gz -C stage
APP="$(cd stage && ls -d */*.app 2>/dev/null | head -1)"
[ -n "$APP" ] || { echo "error: no .app inside the bundle" >&2; exit 1; }
APPNAME="$(basename "$APP")"

mkdir -p root/.background
cp -R "stage/$APP" root/
cp bg.png root/.background/bg.png
ln -s /Applications root/Applications

# Size it from the payload with room for the filesystem's own overhead; too
# small fails at copy time, and hdiutil cannot grow a UDRW image in place.
NEED=$(( $(du -sk root | cut -f1) + 20000 ))
hdiutil create -srcfolder root -volname "$VOLNAME" -fs HFS+ \
    -format UDRW -size ${NEED}k -ov rw.dmg >/dev/null

MNT="/Volumes/$VOLNAME"
# Detach until it is really gone. A volume of this name left over from an
# earlier run makes the Finder answer -10006 ("can't set disk to icon view"),
# because `disk "<name>"` then resolves to the stale read-only one, which has no
# container window to arrange. One detach is not always enough.
for _ in 1 2 3; do
    [ -d "$MNT" ] || break
    hdiutil detach "$MNT" -force >/dev/null 2>&1 || true
    sleep 1
done
[ -d "$MNT" ] && { echo "error: $MNT is still mounted; unmount it and retry" >&2; exit 1; }

hdiutil attach rw.dmg -mountpoint "$MNT" -nobrowse >/dev/null
# The Finder needs a moment to notice the volume before it will hand out a
# container window.
sleep 3

# The Finder writes the layout into the volume's .DS_Store, so this has to
# happen while the image is mounted read/write, and the file has to be flushed
# before it is detached -- hence the update and the pause.
osascript <<AS >/dev/null
tell application "Finder"
    tell disk "$VOLNAME"
        open
        delay 1
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        -- Height allows for the title bar AND Leopard's status bar. Asking for
        -- the status bar to be hidden (above) does not always take, and when it
        -- shows it eats 22px off the top of the content -- which pushed the
        -- background down and clipped its last line. Sizing for the bar being
        -- present costs a blank strip when it is not; the other way round
        -- clips artwork, which looks broken rather than plain.
        set bounds of container window to {150, 120, $((150 + WIN_W)), $((120 + WIN_H + 22 + 22))}
        set opts to the icon view options of container window
        set arrangement of opts to not arranged
        set icon size of opts to 96
        set text size of opts to 12
        set background picture of opts to file ".background:bg.png"
        set position of item "$APPNAME" of container window to {$APP_X, $ICON_Y}
        set position of item "Applications" of container window to {$DEST_X, $ICON_Y}
        update without registering applications
        delay 2
        close
    end tell
end tell
AS
sync

hdiutil detach "$MNT" >/dev/null
# UDZO is what a download should be: compressed and read-only.
hdiutil convert rw.dmg -format UDZO -imagekey zlib-level=9 -o out.dmg >/dev/null

# It must mount and contain what it claims. A .dmg that fails to open is the one
# failure a user cannot work around.
hdiutil attach out.dmg -mountpoint "$MNT" -nobrowse -readonly >/dev/null
[ -d "$MNT/$APPNAME" ] || { echo "error: the app is missing from the image" >&2; exit 1; }
[ -e "$MNT/Applications" ] || { echo "error: no Applications alias in the image" >&2; exit 1; }
[ -f "$MNT/.background/bg.png" ] || { echo "error: no background in the image" >&2; exit 1; }
[ -f "$MNT/.DS_Store" ] || echo "warning: no .DS_Store -- the window will open unarranged" >&2
echo "  image mounts, holds $APPNAME, the Applications alias and the background"
hdiutil detach "$MNT" >/dev/null
REMOTE_SH

mkdir -p "$(dirname "$OUTDMG")"
scp -q "$HOST:$REMOTE/out.dmg" "$OUTDMG"
ssh "$HOST" "rm -rf '$REMOTE'"
echo "wrote $OUTDMG ($(du -h "$OUTDMG" | cut -f1))"
