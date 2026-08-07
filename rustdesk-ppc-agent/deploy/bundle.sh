#!/usr/bin/env bash
#
# Build a self-contained "RustDesk Agent.app" for a PowerPC Mac.
#
#   ./deploy/bundle.sh              # bundle target/ppc/rustdesk-agent
#   PPC_HOST=g4tiger ./deploy/bundle.sh
#
# Produces  target/rustdesk-agent-<arch>.tar.gz  containing an application
# bundle: the binary, every non-system library it needs, the LaunchAgent
# template and install.sh, all under Contents/Resources. Untar it on any
# PowerPC Mac running 10.4 or 10.5, drag the app where you want it, and
# double-click -- it offers to install the background service, and afterwards
# it is where the password and server settings live.
#
# For a headless install over ssh, install.sh is still there:
#   "RustDesk Agent.app/Contents/Resources/install.sh" --password x --yes
#
# WHY THIS EXISTS: the binary is not portable as built. `otool -L` on it names
# five MacPorts libraries by absolute path -- /opt/local/lib/libzstd, libz,
# libMacportsLegacySupport, and gcc10-bootstrap's libatomic and libgcc_s --
# which exist only on a machine somebody has built a toolchain on. libsodium,
# libvpx, libopus and libyuv are *static* and need nothing. So the job is to
# copy those five (plus the two libgcc_s pulls in itself), rewrite the load
# commands to @executable_path, and hand the result over.
#
# The relocation runs ON THE MAC, because install_name_tool is part of Darwin's
# cctools and there is no build of it here. That is also why this script needs
# ssh to a PowerPC machine even though it produces a file on the host: the same
# arrangement build-ppc.sh already uses for the C compiler.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${PPC_HOST:-ppctiger}"
BIN="${PPC_BIN:-$HERE/target/ppc/rustdesk-agent}"
OUT_DIR="${PPC_BUNDLE_OUT:-$HERE/target}"
export SSH_AUTH_SOCK="${SSH_AUTH_SOCK:-/tmp/ssh-agent-ppc.sock}"

[ -f "$BIN" ] || { echo "error: no binary at $BIN -- run ./build-ppc.sh first" >&2; exit 1; }

# The Mach-O cpusubtype decides which machines will load this at all, so the
# tarball is named for it rather than for "ppc". 100 is CPU_SUBTYPE_POWERPC_970
# (G5 only); 10 is 7450 (G4); 0 is ALL. See README, "Running it on a G4".
#
# Read a byte at a time and reassembled big-endian by hand, because the header
# is big-endian and this script runs on a little-endian host: `od -tu4` would
# read 100 as 1677721600, which is a wrong answer rather than an error.
mach_cpusubtype() {
    # `set --` word-splits, so a trailing blank line from BSD od is harmless
    # here; the awk forms elsewhere need NR==1 because they do not.
    set -- $(od -An -tu1 -j8 -N4 "$1")
    echo $(( $1 * 16777216 + $2 * 65536 + $3 * 256 + $4 ))
}
SUBTYPE="$(mach_cpusubtype "$BIN")"
case "$SUBTYPE" in
    100) ARCH=g5 ;;
    10)  ARCH=g4 ;;
    0)   ARCH=ppc ;;
    *)   ARCH="ppc-subtype$SUBTYPE" ;;
esac
case "$ARCH" in
    g5)  ARCH_LABEL="G5" ;;
    g4)  ARCH_LABEL="G4" ;;
    g3)  ARCH_LABEL="G3" ;;
    *)   ARCH_LABEL="$ARCH" ;;
esac
STAGE_REMOTE="/tmp/rd-bundle-$$"
NAME="rustdesk-agent-$ARCH"
TARBALL="$OUT_DIR/$NAME.tar.gz"

# Say which binary, and when it was built. The default is target/ppc, while
# release.sh builds into target/ppc-<arch> -- so an ad-hoc `bundle.sh` after a
# `release.sh` happily packages whatever `build-ppc.sh` left behind last, which
# may be days older than the source. That is not hypothetical: it shipped an
# agent with no --relay-server into an app whose window had a relay field, and
# the only symptom was the window saying "Nothing was changed".
echo "bundling $BIN"
echo "  built $(date -r "$BIN" '+%Y-%m-%d %H:%M'), cpusubtype $SUBTYPE -> $ARCH, via $HOST"

# Everything the target needs that is not the binary itself. Sent first so the
# remote half is a single ssh round trip.
scp -q "$BIN" "$HERE/deploy/install.sh" "$HERE/deploy/agent-ctl.sh" \
       "$HERE/deploy/com.rustdesk.ppc-agent.plist.in" \
       "$HERE/deploy/agent-helper.sh" "$HERE/deploy/app-ui.m" "$HERE/deploy/app.icns" \
       "$HOST:/tmp/"

# The version stamped into the app bundle. Read here rather than on the Mac,
# which has no copy of Cargo.toml.
APP_VERSION="$(sed -n 's/^version = "\(.*\)"/\1/p' "$HERE/Cargo.toml" | head -1)"

# ---------------------------------------------------------------------------
# The remote half: collect, relocate, verify, tar.
#
# `otool -L` is walked transitively rather than once, because libgcc_s.1.dylib
# is a stub that pulls in libgcc_s.1.1 and libgcc_ehs.1.1 -- a one-level copy
# produces a bundle that links and then fails to load.
# ---------------------------------------------------------------------------
ssh "$HOST" "STAGE='$STAGE_REMOTE' NAME='$NAME' APP_VERSION='$APP_VERSION' ARCH_LABEL='$ARCH_LABEL' bash -s" <<'REMOTE'
set -euo pipefail
rm -rf "$STAGE"

# The artifact is an application bundle, because that is how someone installs
# software on a Mac: drag it across, double-click it, and it offers to install
# the service. Everything the installer needs lives in Contents/Resources, and
# the payload sits directly in Resources so that @executable_path/lib -- which
# is relative to the *binary*, not to the bundle -- resolves.
# The UI is a small Cocoa app, compiled here because this is the only PowerPC
# Mac available and Apple's gcc 4.0.1 lives on it. Built with NO -mcpu: it is a
# settings window with nothing to optimise, and a generic-ppc build means the
# same UI binary is correct on a G4 and a G5 -- only the agent differs.
#
# A shell script as CFBundleExecutable was tried first and is not viable:
# LaunchServices refuses it with -10810. A real Mach-O has no such problem.
mkdir -p "$STAGE/$NAME"
APP="$STAGE/$NAME/Agent for RustDesk PPC.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/lib"
D="$APP/Contents/Resources"

gcc -O2 -Wall -o "$APP/Contents/MacOS/AgentForRustDeskPPC" /tmp/app-ui.m -framework Cocoa \
    || { echo "error: the settings app did not compile" >&2; exit 1; }

cp /tmp/rustdesk-agent "$D/rustdesk-agent"
cp /tmp/install.sh "$D/install.sh"
cp /tmp/agent-ctl.sh "$D/rustdesk-ctl"
cp /tmp/com.rustdesk.ppc-agent.plist.in "$D/com.rustdesk.ppc-agent.plist.in"
cp /tmp/agent-helper.sh "$D/agent-helper.sh"
cp /tmp/app.icns "$D/app.icns"
chmod +x "$D/rustdesk-agent" "$D/install.sh" "$D/rustdesk-ctl" "$D/agent-helper.sh"
# Written rather than worked out at runtime: the app should be able to say which
# CPU it carries without re-deriving it from the Mach-O header on the target.
printf '%s' "$ARCH_LABEL" > "$D/BUILD-ARCH"

# Hand-written rather than edited afterwards, so every key is visible in one
# place. NSPrincipalClass with no NSMainNibFile is what makes a nib-less Cocoa
# app start: main() builds the menu bar and window itself.
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>       <string>AgentForRustDeskPPC</string>
    <key>CFBundleIdentifier</key>       <string>com.rustdesk.ppc-agent.settings</string>
    <key>CFBundleName</key>             <string>Agent for RustDesk PPC</string>
    <key>CFBundleIconFile</key>         <string>app.icns</string>
    <key>CFBundleDisplayName</key>      <string>Agent for RustDesk PPC</string>
    <key>CFBundlePackageType</key>      <string>APPL</string>
    <key>CFBundleSignature</key>        <string>????</string>
    <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
    <key>CFBundleVersion</key>          <string>$APP_VERSION</string>
    <key>CFBundleShortVersionString</key> <string>$APP_VERSION ($ARCH_LABEL)</string>
    <key>CFBundleGetInfoString</key>    <string>$APP_VERSION for $ARCH_LABEL</string>
    <key>NSPrincipalClass</key>         <string>NSApplication</string>
    <key>LSMinimumSystemVersion</key>   <string>10.4.0</string>
    <key>NSHumanReadableCopyright</key> <string>RustDesk agent for PowerPC Mac OS X</string>
</dict>
</plist>
PLIST
printf 'APPL????' > "$APP/Contents/PkgInfo"
plutil -lint "$APP/Contents/Info.plist" >/dev/null \
    || { echo "error: Info.plist is malformed" >&2; exit 1; }

# A dependency worth copying: anything not under /usr/lib or /System, i.e. not
# shipped with the OS. Everything else is guaranteed present on any 10.4/10.5.
needs_copy() {
    case "$1" in
        /usr/lib/*|/System/*|@*) return 1 ;;
        *) return 0 ;;
    esac
}

# Breadth-first over the dependency graph. `tail -n +2` drops otool's first
# line, which is the file's own install name rather than a dependency.
declare -a QUEUE
QUEUE=("$D/rustdesk-agent")
COPIED=""
while [ ${#QUEUE[@]} -gt 0 ]; do
    f="${QUEUE[0]}"; QUEUE=("${QUEUE[@]:1}")
    while read -r dep _; do
        [ -n "$dep" ] || continue
        needs_copy "$dep" || continue
        base="$(basename "$dep")"
        case " $COPIED " in *" $base "*) continue ;; esac
        # -L so a symlink (libz.1.dylib -> libz.1.3.2.dylib) becomes a real file
        # under the name the load command actually asks for.
        cp -L "$dep" "$D/lib/$base"
        chmod u+w "$D/lib/$base"
        COPIED="$COPIED $base"
        QUEUE+=("$D/lib/$base")
        echo "  bundled $base"
    done < <(otool -L "$f" | tail -n +2 | sed 's/^[[:space:]]*//')
done

# Rewrite every absolute reference to sit beside the binary. @executable_path
# rather than @loader_path or @rpath: the other two are 10.5+, and this has to
# work on Tiger.
relocate() {
    local target="$1" prefix="$2"
    while read -r dep _; do
        [ -n "$dep" ] || continue
        needs_copy "$dep" || continue
        install_name_tool -change "$dep" "$prefix/$(basename "$dep")" "$target"
    done < <(otool -L "$target" | tail -n +2 | sed 's/^[[:space:]]*//')
}
relocate "$D/rustdesk-agent" "@executable_path/lib"
for l in "$D"/lib/*.dylib; do
    [ -e "$l" ] || continue
    # A library's own install name is what *other* binaries record, so it has to
    # be rewritten too or the next link records /opt/local again.
    install_name_tool -id "@executable_path/lib/$(basename "$l")" "$l"
    relocate "$l" "@executable_path/lib"
done

# Verify rather than assume: nothing outside the OS may remain absolute.
# `tail -n +2` per file, not once over the concatenation -- otool's first line
# is the file's own path with a colon on it, and without dropping each of them
# the check reports the bundle's own staging directory as an unresolved
# dependency, which is what it did the first time this ran.
deps_of_everything() {
    otool -L "$D/rustdesk-agent" | tail -n +2
    for l in "$D"/lib/*.dylib; do
        [ -e "$l" ] || continue
        otool -L "$l" | tail -n +2
    done
}
LEFT="$(deps_of_everything | sed 's/^[[:space:]]*//' | awk '{print $1}' \
         | grep -v '^/usr/lib/' | grep -v '^/System/' | grep -v '^@executable_path/' \
         | grep '^/' | sort -u || true)"
if [ -n "$LEFT" ]; then
    echo "error: these stayed absolute and would not resolve on another machine:" >&2
    echo "$LEFT" >&2
    exit 1
fi

# The strongest check available here: run it. `--show-id` touches the config,
# the crypto and every linked library without needing a window server, so a
# missing or mis-relocated dylib fails now rather than on a user's machine.
if ! ( cd "$D" && ./rustdesk-agent --config "$STAGE/probe.conf" --show-id >/dev/null 2>"$STAGE/err" ); then
    echo "error: the bundled binary does not run:" >&2
    cat "$STAGE/err" >&2
    exit 1
fi
echo "  bundle runs: $( cd "$D" && ./rustdesk-agent --config "$STAGE/probe.conf" --show-id )"
rm -f "$STAGE/probe.conf" "$STAGE/err"

# Every setting the window drives has to exist in the binary being packaged.
# This is the check that would have caught the stale-binary case above: the app
# and the agent ship together, so a flag the helper calls and the agent does not
# know is a packaging fault, catchable here and nowhere else.
# `--help` exits 2 (it is the usage path), and this script runs under
# `set -o pipefail`, so piping it straight into grep reports failure whatever
# grep found. Capture once, then match.
AGENT_HELP="$("$D/rustdesk-agent" --help 2>&1 || true)"
for flag in --password --server --no-server --key --relay-server --show-id --show-key; do
    echo "$AGENT_HELP" | grep -q -- "$flag" || {
        echo "error: the agent being packaged does not support $flag." >&2
        echo "       It is older than this app -- rebuild it before bundling:" >&2
        echo "         ./build-ppc.sh          (writes target/ppc)" >&2
        echo "       or pass PPC_BIN=... to bundle the one you meant." >&2
        exit 1
    }
done
echo "  agent supports every setting the window offers"

# A malformed icns does not error -- the Finder just shows the blank-page
# placeholder, which looks exactly like having forgotten the key. Check the
# container parses and that the entry Leopard prefers is present.
[ -s "$D/app.icns" ] || { echo "error: no icon in the bundle" >&2; exit 1; }
head -c 4 "$D/app.icns" | grep -q icns || { echo "error: app.icns is not an icns" >&2; exit 1; }
grep -q it32 "$D/app.icns" || { echo "error: app.icns has no 128x128 entry" >&2; exit 1; }
echo "  icon present ($(wc -c < "$D/app.icns" | tr -d " ") bytes)"

# The app's non-interactive half, which is everything except the dialogs: it
# reads the config, finds the binary and formats the status block. A typo in any
# of that fails here rather than as an empty window on someone's Mac.
if ! sh "$D/agent-helper.sh" status > "$STAGE/status" 2>&1; then
    echo "error: the app's status command failed:" >&2; cat "$STAGE/status" >&2; exit 1
fi
grep -q '^Status:' "$STAGE/status" || { echo "error: unexpected status output:" >&2; cat "$STAGE/status" >&2; exit 1; }

# State-independent: the build machine usually *has* the agent installed, so a
# check for "Install" passes or fails depending on who is building. Assert what
# is true in both branches -- the settings are offered, and exactly one of
# Install/Uninstall.
MENU="$(sh "$D/agent-helper.sh" menu)"
echo "$MENU" | grep -q '^Set the password$' \
    || { echo "error: the app menu is missing its settings items:" >&2; echo "$MENU" >&2; exit 1; }
N_STATE="$(echo "$MENU" | grep -c '^Install$\|^Uninstall$')"
[ "$N_STATE" = "1" ] \
    || { echo "error: menu offers $N_STATE of Install/Uninstall, expected exactly 1" >&2; echo "$MENU" >&2; exit 1; }

# The UI binary must be a Mach-O (LaunchServices refuses scripts) and must not
# be stamped 970, or the G4 download would ship a settings window that will not
# start on a G4. Apple's gcc 4.0.1 stamps 10 (7450) by default with no -mcpu at
# all -- not 0 as expected -- and 10 runs on both a G4 and a G5, so both 0 and
# 10 are accepted and only 100 is refused.
EXE="$APP/Contents/MacOS/AgentForRustDeskPPC"
file "$EXE" | grep -q 'Mach-O' || { echo "error: the UI is not a Mach-O" >&2; exit 1; }
# NR==1/exit: BSD od emits a trailing blank line -- see install.sh.
UI_SUB="$(od -An -tu1 -j8 -N4 "$EXE" | awk 'NR==1 { print $1 * 16777216 + $2 * 65536 + $3 * 256 + $4; exit }')"
case "$UI_SUB" in
    0|10) ;;
    *) echo "error: the UI binary is cpusubtype $UI_SUB; it must be 0 or 10 so that" >&2
       echo "       the same settings window works on a G4 and a G5" >&2; exit 1 ;;
esac
echo "  app: Cocoa UI compiles (cpusubtype $UI_SUB), helper status and menu work"
rm -f "$STAGE/status"

( cd "$STAGE" && tar czf "$NAME.tar.gz" "$NAME" )
REMOTE

mkdir -p "$OUT_DIR"
scp -q "$HOST:$STAGE_REMOTE/$NAME.tar.gz" "$TARBALL"
ssh "$HOST" "rm -rf '$STAGE_REMOTE' /tmp/rustdesk-agent /tmp/install.sh /tmp/agent-ctl.sh /tmp/com.rustdesk.ppc-agent.plist.in"

echo
echo "built $TARBALL ($(du -h "$TARBALL" | cut -f1))"
echo
cat <<EOM

To install on a PowerPC Mac:
    scp $TARBALL user@mac:~/
    then on the Mac: untar it, drag "Agent for RustDesk PPC" into
    /Applications (or anywhere), and double-click it. It offers to
    install the background service.

Or headless, over ssh:
    ssh user@mac "tar xzf $NAME.tar.gz && sh '$NAME/Agent for RustDesk PPC.app/Contents/Resources/install.sh' --yes --password <pw>"
EOM
