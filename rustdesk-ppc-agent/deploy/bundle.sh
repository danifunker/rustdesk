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

echo "bundling $(basename "$BIN") (cpusubtype $SUBTYPE -> $ARCH) via $HOST"

# Everything the target needs that is not the binary itself. Sent first so the
# remote half is a single ssh round trip.
scp -q "$BIN" "$HERE/deploy/install.sh" "$HERE/deploy/agent-ctl.sh" \
       "$HERE/deploy/com.rustdesk.ppc-agent.plist.in" \
       "$HERE/deploy/agent-helper.sh" "$HERE/deploy/app.applescript" \
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
# The UI is a COMPILED APPLET, not a shell script as CFBundleExecutable.
# LaunchServices refuses the latter with -10810 once it is any bigger than
# trivial, and even when it launches only its first osascript can display
# anything. osacompile produces a real application with Apple's own Mach-O
# executable, which has neither problem. See app.applescript's header.
mkdir -p "$STAGE/$NAME"
APP="$STAGE/$NAME/Agent for RustDesk PPC.app"
osacompile -o "$APP" /tmp/app.applescript \
    || { echo "error: the app's AppleScript does not compile" >&2; exit 1; }

# The payload sits directly in Resources so that @executable_path/lib -- which
# is relative to the *binary being run*, not to the bundle -- resolves when the
# agent is launched from there before installation.
D="$APP/Contents/Resources"
mkdir -p "$D/lib"
cp /tmp/rustdesk-agent "$D/rustdesk-agent"
cp /tmp/install.sh "$D/install.sh"
cp /tmp/agent-ctl.sh "$D/rustdesk-ctl"
cp /tmp/com.rustdesk.ppc-agent.plist.in "$D/com.rustdesk.ppc-agent.plist.in"
cp /tmp/agent-helper.sh "$D/agent-helper.sh"
# Written rather than worked out at runtime: the app should be able to say which
# CPU it carries without re-deriving it from the Mach-O header on the target.
printf '%s' "$ARCH_LABEL" > "$D/BUILD-ARCH"
chmod +x "$D/rustdesk-agent" "$D/install.sh" "$D/rustdesk-ctl" "$D/agent-helper.sh"

# osacompile names every applet "Applet". Give it ours.
# -string on every one of these: without it `defaults` parses the value as a
# plist expression, and "0.1.0 (G5)" fails with "Could not parse".
defaults write "$APP/Contents/Info" CFBundleName -string "Agent for RustDesk PPC"
defaults write "$APP/Contents/Info" CFBundleDisplayName -string "Agent for RustDesk PPC"
defaults write "$APP/Contents/Info" CFBundleIdentifier -string "com.rustdesk.ppc-agent.settings"
defaults write "$APP/Contents/Info" CFBundleVersion -string "$APP_VERSION"
# The G4 and G5 builds are separate downloads with the same app name, so the CPU
# goes in the version string and in Get Info: once both are dragged out of their
# folders there is otherwise nothing to tell them apart, and installing the
# wrong one only fails at install time.
defaults write "$APP/Contents/Info" CFBundleShortVersionString -string "$APP_VERSION ($ARCH_LABEL)"
defaults write "$APP/Contents/Info" CFBundleGetInfoString -string "$APP_VERSION for $ARCH_LABEL"
plutil -lint "$APP/Contents/Info.plist" >/dev/null \
    || { echo "error: Info.plist is malformed after editing" >&2; exit 1; }

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

# And the applet really is an application: a Mach-O executable, not a script.
EXE="$(defaults read "$APP/Contents/Info" CFBundleExecutable)"
file "$APP/Contents/MacOS/$EXE" | grep -q 'Mach-O' \
    || { echo "error: the app executable is not a Mach-O; osacompile did not run?" >&2; exit 1; }
echo "  app: applet compiles, executable is Mach-O, status and menu work"
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
