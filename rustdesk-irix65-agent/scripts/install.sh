#!/bin/sh
# Install the RustDesk agent by hand, from the tarball, on the machine itself.
#
# The Software Manager package (.tardist) is the normal way in and does the same
# thing; this is for a machine where inst is not available or not wanted, and it
# is deliberately readable so that anyone can see exactly what lands where.
#
# Bourne shell only -- /bin/sh on IRIX is the SVR4 shell. No $( ), no `local`.
#
#   sh install.sh                 install into the standard places
#   sh install.sh -p /opt/rd      install under a different prefix
#   sh install.sh -u              remove what an install put there
#
# WHAT GOES WHERE
#   $PREFIX/sbin/rustdesk-agent              the agent
#   $PREFIX/sbin/rustdesk-agent-gui          the settings panel
#   $PREFIX/lib/rustdesk-agent/agent-helper.sh
#   $PREFIX/lib/rustdesk-agent/libgcc_s.so.1
#   /usr/lib/X11/app-chests/RustDesk.chest   Toolchest entry (only for /usr)
#
# libgcc_s.so.1 is the one library the agent needs that IRIX 6.5 does not ship.
# The binary carries an rpath of /usr/lib/rustdesk-agent, so a default install
# needs no environment variable at all. Under a DIFFERENT prefix the rpath no
# longer points at the copy, and the wrapper this script writes is what bridges
# that -- which is the whole reason a non-default prefix is worth mentioning
# rather than silently supported.
set -u

PREFIX=/usr
UNINSTALL=0
SRC=`dirname "$0"`

while [ $# -gt 0 ]; do
	case "$1" in
		-p) PREFIX="$2"; shift 2 ;;
		-u) UNINSTALL=1; shift ;;
		-h) sed -n '2,28p' "$0"; exit 0 ;;
		*)  echo "install.sh: unknown option: $1" >&2; exit 2 ;;
	esac
done

BIN="$PREFIX/sbin"
LIB="$PREFIX/lib/rustdesk-agent"
CHEST=/usr/lib/X11/app-chests/RustDesk.chest

if [ "$UNINSTALL" = 1 ]; then
	echo "Stopping the agent, if it is running."
	# Match the BASENAME OF argv[0] exactly. `ps -e` truncates the command to
	# eight characters on IRIX so it cannot be used at all, and grepping the
	# full command line matches too much: the settings panel
	# (rustdesk-agent-gui), and this script itself when it is run out of a
	# directory called rustdesk-agent-VERSION-n32, which is exactly what the
	# tarball unpacks to.
	for p in `ps -e -o pid,args | awk '{ c = $2; sub(/.*\//, "", c); if (c == "rustdesk-agent") print $1 }'`; do
		kill "$p" 2>/dev/null
	done
	rm -f "$BIN/rustdesk-agent" "$BIN/rustdesk-agent-gui"
	# Under a non-default prefix the real binaries live in $LIB and the ones in
	# $BIN are wrappers, so both places have to go or `rmdir` below quietly
	# fails on a directory that still holds a 7 MB agent.
	rm -f "$LIB/rustdesk-agent" "$LIB/rustdesk-agent-gui"
	rm -f "$LIB/agent-helper.sh" "$LIB/libgcc_s.so.1"
	rmdir "$LIB" 2>/dev/null
	rm -f "$CHEST"
	echo "Removed. The configuration in ~/.rustdesk-ppc-agent.conf is left alone;"
	echo "delete it by hand if you want the machine's identity gone too."
	exit 0
fi

id | grep -q 'uid=0' || { echo "install.sh: run this as root." >&2; exit 1; }

for f in bin/rustdesk-agent bin/rustdesk-agent-gui lib/agent-helper.sh lib/libgcc_s.so.1; do
	[ -f "$SRC/$f" ] || { echo "install.sh: missing $SRC/$f" >&2; exit 1; }
done

mkdir -p "$BIN" "$LIB" || exit 1

echo "Installing into $PREFIX."
cp "$SRC/bin/rustdesk-agent"      "$BIN/rustdesk-agent"      || exit 1
cp "$SRC/bin/rustdesk-agent-gui"  "$BIN/rustdesk-agent-gui"  || exit 1
cp "$SRC/lib/agent-helper.sh"     "$LIB/agent-helper.sh"     || exit 1
cp "$SRC/lib/libgcc_s.so.1"       "$LIB/libgcc_s.so.1"       || exit 1
chmod 755 "$BIN/rustdesk-agent" "$BIN/rustdesk-agent-gui" \
          "$LIB/agent-helper.sh" "$LIB/libgcc_s.so.1"

# The Toolchest entry, only when installing where the desktop looks. A fragment
# in app-chests edits no system file, and f.checkexec.sh means the entry hides
# itself as soon as the program is gone -- so removing it needs no hook.
if [ "$PREFIX" = /usr ] && [ -d /usr/lib/X11/app-chests ]; then
	if [ -f "$SRC/chest/RustDesk.chest" ]; then
		cp "$SRC/chest/RustDesk.chest" "$CHEST" && chmod 644 "$CHEST"
		echo "Toolchest entry: $CHEST (log out and back in to see it)."
	fi
fi

# A non-default prefix breaks two lookups that are compiled in, and both are
# bridged the same honest way -- with a wrapper that sets the variable -- rather
# than by rewriting a linked path behind anyone's back:
#
#   the agent  carries an rpath of /usr/lib/rustdesk-agent, which is not where
#              its libgcc_s.so.1 ended up
#   the panel  searches beside argv[0] and then /usr/lib/rustdesk-agent for the
#              helper, and finds neither. RD_HELPER overrides that search.
if [ "$PREFIX" != /usr ]; then
	mv "$BIN/rustdesk-agent" "$LIB/rustdesk-agent" || exit 1
	cat > "$BIN/rustdesk-agent" <<EOF
#!/bin/sh
# Written by install.sh for a non-default prefix.
LD_LIBRARYN32_PATH="$LIB:\${LD_LIBRARYN32_PATH:-}"
export LD_LIBRARYN32_PATH
exec "$LIB/rustdesk-agent" "\$@"
EOF
	chmod 755 "$BIN/rustdesk-agent"

	mv "$BIN/rustdesk-agent-gui" "$LIB/rustdesk-agent-gui" || exit 1
	cat > "$BIN/rustdesk-agent-gui" <<EOF
#!/bin/sh
# Written by install.sh for a non-default prefix.
RD_HELPER="$LIB/agent-helper.sh"
RD_AGENT="$BIN/rustdesk-agent"
export RD_HELPER RD_AGENT
exec "$LIB/rustdesk-agent-gui" "\$@"
EOF
	chmod 755 "$BIN/rustdesk-agent-gui"
	echo "Wrote wrappers in $BIN (non-default prefix)."
fi

echo
echo "Installed:"
ls -l "$BIN/rustdesk-agent" "$BIN/rustdesk-agent-gui" "$LIB/agent-helper.sh" "$LIB/libgcc_s.so.1"
echo
echo "Next:"
echo "  $BIN/rustdesk-agent --show-id        what this machine's ID is"
echo "  $BIN/rustdesk-agent-gui              the settings panel (needs a display)"
echo "  $LIB/agent-helper.sh start           start it now"
