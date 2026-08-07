#!/bin/sh
#
# Install the RustDesk PowerPC agent for the current user.
#
#   ./install.sh                                  # interactive
#   ./install.sh --password hunter2 --yes         # unattended
#   ./install.sh --server rd.example.org --key "<base64>" --password hunter2 --yes
#   ./install.sh --uninstall
#
# /bin/sh rather than bash, and POSIX rather than bashisms: this runs on Tiger,
# whose /bin/sh is bash 2.05b, and on machines where nothing has been installed
# yet. No MacPorts, no python, no perl beyond what Apple ships.
#
# Everything it touches lives under $HOME. No sudo, no /usr/local, nothing
# system-wide -- which is not only politeness: the agent *must* run in the
# user's Aqua session to reach the pasteboard and the window server, so a
# system-wide daemon would be the wrong thing even if it were easier.
#
# **One installation per machine**, even with different --prefix values. The
# LaunchAgent label is fixed, and `launchctl unload` resolves a plist to its
# label rather than to its path -- so uninstalling one copy stops whichever
# copy currently holds the label. Found by installing a second copy under a
# different HOME to test this script, which duly stopped the first one.
# Re-running the installer to upgrade in place is the supported way.
set -eu

# Exact match on the label, not a substring. `launchctl list` prints
# PID<TAB>status<TAB>label, and the settings app registers itself as
# "[0x0-...].com.rustdesk.ppc-agent.settings" -- which CONTAINS the agent's
# label, so `grep -q "$LABEL"` is true whenever the window is merely open. That
# made Stop report "It is still running" every time, and the Start/Stop buttons
# read the wrong state. The app's bundle id being a prefix of the job label is
# allowed; code that cannot tell them apart is not.
LABEL=com.rustdesk.ppc-agent
PREFIX="$HOME/rustdesk-ppc-agent"
PORT=21118
PASSWORD=""
SERVER=""
SERVER_KEY=""
ASSUME_YES=0
UNINSTALL=0
SET_SERVER=0
SET_KEY=0

SRC="$(cd "$(dirname "$0")" && pwd)"

die() { echo "error: $*" >&2; exit 1; }
note() { echo "  $*"; }

usage() {
    cat >&2 <<EOF
usage: ./install.sh [options]

  --password PASS   set the connection password (>= 6 characters)
  --server HOST     register with this rendezvous server, so the machine is
                    reachable by ID rather than only by IP on its own subnet
  --key KEY         the server's key, as pasted into a client's "Key" field.
                    Only needed if the relay (hbbr) was started with -k
  --port N          listen port (default $PORT)
  --prefix DIR      install location (default $PREFIX)
  --yes             do not prompt; fail instead of asking
  --uninstall       stop the agent and remove everything this installed
  --help            this
EOF
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --password) [ $# -ge 2 ] || usage; PASSWORD="$2"; shift 2 ;;
        --server)   [ $# -ge 2 ] || usage; SERVER="$2"; SET_SERVER=1; shift 2 ;;
        --key)      [ $# -ge 2 ] || usage; SERVER_KEY="$2"; SET_KEY=1; shift 2 ;;
        --port)     [ $# -ge 2 ] || usage; PORT="$2"; shift 2 ;;
        --prefix)   [ $# -ge 2 ] || usage; PREFIX="$2"; shift 2 ;;
        --yes|-y)   ASSUME_YES=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        --help|-h)  usage ;;
        *) echo "unknown option: $1" >&2; usage ;;
    esac
done

PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
BIN="$PREFIX/rustdesk-agent"
LOG="$PREFIX/agent.log"
CTL="$HOME/rustdesk-ctl"

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
if [ "$UNINSTALL" -eq 1 ]; then
    echo "Removing the RustDesk agent."
    # -S Aqua for the same reason `start` needs it: without it an ssh login
    # matches nothing and unload silently leaves the agent running.
    launchctl unload -S Aqua "$PLIST" >/dev/null 2>&1 || true
    launchctl unload "$PLIST" >/dev/null 2>&1 || true
    # Only *this* installation's processes, matched on the full path. Uninstall
    # has no business killing an agent someone else installed elsewhere -- which
    # matching on the process name alone would do.
    ps -axo pid,command | grep -F "$BIN" | grep -v grep \
        | awk '{ print $1 }' | while read p; do kill -9 "$p" 2>/dev/null || true; done
    rm -f "$PLIST" "$CTL"
    rm -rf "$PREFIX"
    note "removed $PREFIX, $PLIST"
    # Deliberately kept: it holds the machine's ID, its signing key and the
    # password, so removing it would change the ID on reinstall and every peer
    # would have to be told the new one.
    if [ -f "$HOME/.rustdesk-ppc-agent.conf" ]; then
        note "kept $HOME/.rustdesk-ppc-agent.conf (the machine's ID and key)"
        note "delete it by hand to get a brand-new identity"
    fi
    echo "Done."
    exit 0
fi

# ---------------------------------------------------------------------------
# Will this even run here? Both checks below produce failures that are hard to
# read if they are left to happen on their own: a cpusubtype mismatch is
# "Bad CPU type in executable" or a bus error, and a missing library is a dyld
# message naming a path the user has never heard of.
# ---------------------------------------------------------------------------
[ -f "$SRC/rustdesk-agent" ] || die "no rustdesk-agent beside this script -- untar the bundle first"

case "$(uname -s)" in
    Darwin) ;;
    *) die "this is a Mac OS X agent; this machine says $(uname -s)" ;;
esac

OSVER="$(sw_vers -productVersion 2>/dev/null || echo unknown)"
case "$OSVER" in
    10.4*|10.5*) ;;
    unknown) note "warning: cannot read the OS version; continuing" ;;
    *) note "warning: built for Mac OS X 10.4/10.5, this is $OSVER -- continuing anyway" ;;
esac


# Which CPU subtypes a Mach-O contains: one for a thin file, several for a
# universal one. A fat file starts 0xcafebabe and its header is nfat_arch at
# offset 4 then 20-byte entries of (cputype, cpusubtype, offset, size, align) --
# so reading offset 8 as "the cpusubtype", which is right for a thin file, gets
# the *first slice's cputype* for a fat one and silently judges the wrong thing.
mach_subtypes() {
    _f="$1"
    _be4() { od -An -tu1 -j"$2" -N4 "$1" | awk 'NR==1 { print $1*16777216 + $2*65536 + $3*256 + $4; exit }'; }
    case "$(od -An -tx1 -N4 "$_f" | tr -d ' \n')" in
        cafebabe)
            _n="$(_be4 "$_f" 4)"
            _i=0
            while [ "$_i" -lt "$_n" ]; do
                _be4 "$_f" $(( 8 + _i * 20 + 4 ))
                _i=$(( _i + 1 ))
            done ;;
        feedface)
            _be4 "$_f" 8 ;;
        *) ;;   # not a Mach-O we know; callers treat an empty list as unknown
    esac
}

# Can this machine execute a slice of that subtype? 0 is ALL, 10/11 are the G4
# pair (7400 and 7450), 100 is the 970. A G5 runs all of them; a G4 runs
# everything except 100.
subtype_runs_here() {
    case "$1:$2" in
        0:*|*:unknown)      return 0 ;;
        100:ppc970)         return 0 ;;
        100:*)              return 1 ;;
        10:*|11:*)          case "$2" in ppc750*|ppc603*|ppc604*) return 1 ;; *) return 0 ;; esac ;;
        *)                  return 0 ;;
    esac
}

CPU="$(machine 2>/dev/null || echo unknown)"
SUBTYPES="$(mach_subtypes "$SRC/rustdesk-agent")"
[ -n "$SUBTYPES" ] || die "$SRC/rustdesk-agent is not a Mach-O binary"

RUNNABLE=0
for st in $SUBTYPES; do
    if subtype_runs_here "$st" "$CPU"; then RUNNABLE=1; fi
done
if [ "$RUNNABLE" -eq 0 ]; then
    die "this bundle holds only cpusubtype(s) [$SUBTYPES] and this is a $CPU.
       It would fail to load. Nothing in the agent is G5-specific -- the CPU
       flag is -- so rebuild on the host with:

           PPC_CPU_FLAGS='-mcpu=7450 -maltivec' ./build-ppc.sh
           ./deploy/bundle.sh

       or use the universal download, which carries both."
fi
case "$SUBTYPES" in
    *\ *) note "universal binary: subtypes $SUBTYPES; this $CPU picks one at launch" ;;
esac

# AltiVec is on every G4 and G5 and on no G3, and the shims are built with it.
case "$CPU" in
    ppc750*|ppc603*|ppc604*)
        note "warning: $CPU is a G3 and has no AltiVec; a build made with"
        note "         -maltivec will fault here. Untested on this hardware." ;;
esac

if [ -e "$PREFIX" ] && [ ! -d "$PREFIX" ]; then
    die "$PREFIX exists and is not a directory (an older install put a bare
       binary there?). Move it aside and run this again."
fi

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
echo "Installing the RustDesk agent for $(id -un) on $(hostname)."
note "from    $SRC"
note "into    $PREFIX"

mkdir -p "$PREFIX" "$HOME/Library/LaunchAgents"

# Stop anything already running before overwriting the binary it is executing.
# KeepAlive means a plain kill is not enough: launchd puts it straight back.
if launchctl list 2>/dev/null | awk -v l="$LABEL" '$NF == l { f = 1 } END { exit !f }' ; then
    note "stopping the running agent"
    launchctl unload -S Aqua "$PLIST" >/dev/null 2>&1 || \
        launchctl unload "$PLIST" >/dev/null 2>&1 || true
    sleep 1
fi
# Broad on purpose here, unlike uninstall: *any* agent still holding the listen
# port stops the new one from binding, including one left behind by an earlier
# install in a different place or by `build-ppc.sh deploy`'s detached screen.
ps -axo pid,comm | awk '$2 ~ /rustdesk-agent/ { print $1 }' \
    | while read p; do kill -9 "$p" 2>/dev/null || true; done

cp "$SRC/rustdesk-agent" "$BIN"
chmod 755 "$BIN"
if [ -d "$SRC/lib" ]; then
    rm -rf "$PREFIX/lib"
    mkdir -p "$PREFIX/lib"
    cp "$SRC"/lib/*.dylib "$PREFIX/lib/" 2>/dev/null || true
    chmod 644 "$PREFIX"/lib/*.dylib 2>/dev/null || true
    note "libraries: $(ls "$PREFIX/lib" | wc -l | tr -d ' ') bundled beside the binary"
fi

# Does it actually run here? Every library, the config and the crypto, without
# needing a window server. Anything wrong shows up now, with dyld's own message,
# rather than as a LaunchAgent that flaps invisibly under KeepAlive.
if ! "$BIN" --show-id >/dev/null 2>"$PREFIX/.install-check"; then
    echo "error: the agent does not run on this machine:" >&2
    sed 's/^/       /' "$PREFIX/.install-check" >&2
    rm -f "$PREFIX/.install-check"
    exit 1
fi
rm -f "$PREFIX/.install-check"

if [ -f "$SRC/rustdesk-ctl" ]; then
    cp "$SRC/rustdesk-ctl" "$PREFIX/rustdesk-ctl"
    chmod 755 "$PREFIX/rustdesk-ctl"
    # A stable name in $HOME, which is what the documentation tells people to
    # type. Written as a wrapper rather than a symlink so the control script
    # finds the binary wherever --prefix put it.
    cat > "$CTL" <<EOF
#!/bin/sh
# Generated by install.sh. Points the control script at this installation.
RUSTDESK_AGENT_BIN="$BIN" RUSTDESK_AGENT_LOG="$LOG" exec "$PREFIX/rustdesk-ctl" "\$@"
EOF
    chmod 755 "$CTL"
fi

# ---------------------------------------------------------------------------
# Configuration. Anything already in the config is left alone unless a flag
# names it, so re-running the installer to upgrade the binary does not wipe the
# password or un-register the machine.
# ---------------------------------------------------------------------------
if [ -z "$PASSWORD" ] && [ "$ASSUME_YES" -eq 0 ]; then
    # The config is `key = value`, so the space is not optional in this pattern.
    if [ -f "$HOME/.rustdesk-ppc-agent.conf" ] \
       && grep -q '^password *=' "$HOME/.rustdesk-ppc-agent.conf" 2>/dev/null; then
        note "a password is already set; leaving it alone"
    else
        echo
        echo "Set a connection password (at least 6 characters)."
        echo "This is what someone types to connect, so make it a real one."
        printf "  password: "
        # No `read -s` on Tiger's sh. stty is the portable way, and the trap
        # matters: a Ctrl-C at the prompt would otherwise leave echo off.
        stty -echo 2>/dev/null || true
        trap 'stty echo 2>/dev/null || true' INT TERM
        read PASSWORD
        stty echo 2>/dev/null || true
        trap - INT TERM
        echo
    fi
fi

if [ -n "$PASSWORD" ]; then
    if [ ${#PASSWORD} -lt 6 ]; then
        die "the password must be at least 6 characters"
    fi
    "$BIN" --password "$PASSWORD" >/dev/null || die "could not save the password"
    note "password set"
fi

if [ "$SET_SERVER" -eq 1 ]; then
    if [ -n "$SERVER" ]; then
        "$BIN" --server "$SERVER" >/dev/null || die "could not save the server"
        note "will register with $SERVER"
    else
        "$BIN" --no-server >/dev/null || die "could not clear the server"
        note "registration disabled; direct IP only"
    fi
fi

if [ "$SET_KEY" -eq 1 ]; then
    "$BIN" --key "$SERVER_KEY" >/dev/null || die "could not save the key"
    if [ -n "$SERVER_KEY" ]; then note "server key set"; else note "server key cleared"; fi
fi

# ---------------------------------------------------------------------------
# The LaunchAgent
# ---------------------------------------------------------------------------
[ -f "$SRC/com.rustdesk.ppc-agent.plist.in" ] || die "no plist template in the bundle"
sed -e "s|@BIN@|$BIN|g" -e "s|@LOG@|$LOG|g" -e "s|@PORT@|$PORT|g" \
    "$SRC/com.rustdesk.ppc-agent.plist.in" > "$PLIST"
plutil -lint "$PLIST" >/dev/null 2>&1 || die "the generated plist is malformed: $PLIST"
note "LaunchAgent written to $PLIST"

# `-S Aqua` is the whole trick and is not in Leopard's usage text: plain
# `launchctl load` filters by the *caller's* session type, so from ssh it finds
# a plist marked LimitLoadToSessionType Aqua, matches nothing, and reports
# "nothing found to load" -- which is true and says nothing about what to do.
launchctl unload -S Aqua "$PLIST" >/dev/null 2>&1 || true
if launchctl load -w -S Aqua "$PLIST" >/dev/null 2>&1; then
    note "loaded into the Aqua session"
else
    launchctl load -w "$PLIST" >/dev/null 2>&1 || true
    note "loaded (without -S Aqua; this launchd may be older)"
fi

sleep 2

echo
if launchctl list 2>/dev/null | awk -v l="$LABEL" '$NF == l { f = 1 } END { exit !f }' ; then
    echo "Running. It will start again at every login."
else
    echo "The LaunchAgent is installed but not running yet."
    echo "If you are on ssh, that can happen when no one is logged in at the Mac."
    echo "Log in there, or run:  $CTL"
fi

ID="$("$BIN" --show-id 2>/dev/null || echo '?')"
KEY="$("$BIN" --show-key 2>/dev/null || echo '?')"
IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '')"

echo
echo "  ID         : $ID"
echo "  public key : $KEY"
if [ -n "$IP" ]; then echo "  address    : $IP  (type this into a client's ID field)"; fi
echo "  log        : $LOG"
echo "  control    : $CTL   (start | stop | status)"
echo
echo "Connections by IP are unencrypted, which is also upstream's behaviour for"
echo "direct connections. Connections by ID through a rendezvous server are"
echo "encrypted."
echo
echo "For the Mac to be reachable after a reboot without someone logging in,"
echo "turn on automatic login: System Preferences -> Accounts -> Login Options."
