#!/bin/sh
# Install R-DeskVint (the RustDesk agent) by hand, from the tarball, on the machine itself.
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
#   $PREFIX/sbin/r-deskvint-irix              the agent: bin/r-deskvint-irix-mips4
#                                            on an R5000, R8000, R10000 or later,
#                                            bin/r-deskvint-irix (MIPS III) on
#                                            anything else -- see below
#   $PREFIX/sbin/r-deskvint-irix-gui          the settings panel
#   $PREFIX/lib/r-deskvint-irix/agent-helper.sh
#   $PREFIX/lib/r-deskvint-irix/libgcc_s.so.1
#   $PREFIX/sbin/cacert.pem                  CA roots for an https console
#   $PREFIX/lib/r-deskvint-irix/r_deskvint_irix.init   the init.d script, NOT
#                                            installed into /etc by this script:
#                                            `agent-helper.sh service install`
#   /usr/lib/X11/app-chests/R-DeskVint.chest   Toolchest entry (default prefix only)
#
# The Toolchest fragment is the one path that is not ours to choose: the desktop
# picks it up through the `sinclude /usr/lib/X11/app-chests` at the end of
# /usr/lib/X11/system.chestrc, so it goes there whatever $PREFIX is -- and
# because it names the panel's absolute path, it is only written for a default
# install, where that path is right.
#
# libgcc_s.so.1 is the one library the agent needs that IRIX 6.5 does not ship.
# The binary carries an rpath of $DEFAULT_PREFIX/lib/r-deskvint-irix, so a
# default install needs no environment variable at all. Under a DIFFERENT prefix the rpath no
# longer points at the copy, and the wrapper this script writes is what bridges
# that -- which is the whole reason a non-default prefix is worth mentioning
# rather than silently supported.
set -u

# Where the build expects to end up: the agent's rpath names it (ports/rust/
# env.sh, RD_RPATH) and the Toolchest fragment names it. Anywhere else works and
# is bridged by the wrappers below, but this is the one that needs neither.
DEFAULT_PREFIX=/usr/local

PREFIX=$DEFAULT_PREFIX
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
LIB="$PREFIX/lib/r-deskvint-irix"
CHEST=/usr/lib/X11/app-chests/R-DeskVint.chest

if [ "$UNINSTALL" = 1 ]; then
	echo "Stopping the agent, if it is running."
	# Match the BASENAME OF argv[0] exactly. `ps -e` truncates the command to
	# eight characters on IRIX so it cannot be used at all, and grepping the
	# full command line matches too much: the settings panel
	# (r-deskvint-irix-gui), and this script itself when it is run out of a
	# directory called r-deskvint-irix-VERSION-n32, which is exactly what the
	# tarball unpacks to.
	for p in `ps -e -o pid,args | awk '{ c = $2; sub(/.*\//, "", c); if (c == "r-deskvint-irix") print $1 }'`; do
		kill "$p" 2>/dev/null
	done
	rm -f "$BIN/r-deskvint-irix" "$BIN/r-deskvint-irix-gui"
	# Under a non-default prefix the real binaries live in $LIB and the ones in
	# $BIN are wrappers, so both places have to go or `rmdir` below quietly
	# fails on a directory that still holds a 7 MB agent.
	rm -f "$LIB/r-deskvint-irix" "$LIB/r-deskvint-irix-gui"
	rm -f "$LIB/agent-helper.sh" "$LIB/libgcc_s.so.1" "$LIB/r_deskvint_irix.init"
	rm -f "$BIN/cacert.pem"
	rmdir "$LIB" 2>/dev/null
	rm -f "$CHEST"
	echo "Removed. The configuration in /etc/r-deskvint-irix.conf is left alone;"
	echo "delete it by hand if you want the machine's identity gone too."
	exit 0
fi

id | grep -q 'uid=0' || { echo "install.sh: run this as root." >&2; exit 1; }

for f in bin/r-deskvint-irix bin/r-deskvint-irix-gui lib/agent-helper.sh lib/libgcc_s.so.1; do
	[ -f "$SRC/$f" ] || { echo "install.sh: missing $SRC/$f" >&2; exit 1; }
done

# Which agent. The tarball carries two builds of it: bin/r-deskvint-irix for
# MIPS III, which every IRIX 6.5 machine can run, and bin/r-deskvint-irix-mips4
# for MIPS IV -- R5000, R8000, R10000 and everything after -- a few percent
# faster. hinv names the CPU. Anything not recognised gets MIPS III, because the
# cost of guessing wrong the other way is an agent that dies of SIGILL. The
# .tardist makes the same choice through inst's CPUARCH (see the idb).
AGENT_SRC="$SRC/bin/r-deskvint-irix"
AGENT_ISA="MIPS III"
if [ -f "$SRC/bin/r-deskvint-irix-mips4" ]; then
	case "`hinv -c processor 2>/dev/null | grep '^CPU'`" in
		*R4[0-9]00*) ;;
		*R5000*|*R8000*|*R1[0-9]000*|*RM5[0-9]*|*RM7[0-9]*)
			AGENT_SRC="$SRC/bin/r-deskvint-irix-mips4"
			AGENT_ISA="MIPS IV" ;;
	esac
fi

mkdir -p "$BIN" "$LIB" || exit 1

echo "Installing into $PREFIX."
echo "Agent: the $AGENT_ISA build (`hinv -c processor 2>/dev/null | sed -n 's/^CPU: //p'`)."
cp "$AGENT_SRC"                   "$BIN/r-deskvint-irix"      || exit 1
cp "$SRC/bin/r-deskvint-irix-gui"  "$BIN/r-deskvint-irix-gui"  || exit 1
cp "$SRC/lib/agent-helper.sh"     "$LIB/agent-helper.sh"     || exit 1
cp "$SRC/lib/libgcc_s.so.1"       "$LIB/libgcc_s.so.1"       || exit 1
if [ -f "$SRC/lib/r_deskvint_irix.init" ]; then
	cp "$SRC/lib/r_deskvint_irix.init" "$LIB/r_deskvint_irix.init" && chmod 755 "$LIB/r_deskvint_irix.init"
fi

# Beside the binary, which is where the agent looks first. Optional, because a
# build made on a host with no bundle still installs and runs -- it just cannot
# verify an https console until --ca-bundle names one.
if [ -f "$SRC/bin/cacert.pem" ]; then
	cp "$SRC/bin/cacert.pem" "$BIN/cacert.pem" && chmod 644 "$BIN/cacert.pem"
	echo "Certificates: $BIN/cacert.pem (`grep -c 'BEGIN CERTIFICATE' "$BIN/cacert.pem"` roots, for an https console)"
else
	echo "No cacert.pem in this build -- an https console will need --ca-bundle."
fi
chmod 755 "$BIN/r-deskvint-irix" "$BIN/r-deskvint-irix-gui" \
          "$LIB/agent-helper.sh" "$LIB/libgcc_s.so.1"

# The Toolchest entry, only when installing where the desktop looks. A fragment
# in app-chests edits no system file, and f.checkexec.sh means the entry hides
# itself as soon as the program is gone -- so removing it needs no hook.
if [ "$PREFIX" = "$DEFAULT_PREFIX" ] && [ -d /usr/lib/X11/app-chests ]; then
	if [ -f "$SRC/chest/R-DeskVint.chest" ]; then
		cp "$SRC/chest/R-DeskVint.chest" "$CHEST" && chmod 644 "$CHEST"
		echo "Toolchest entry: $CHEST (log out and back in to see it)."
	fi
fi

# A non-default prefix breaks two lookups that are compiled in, and both are
# bridged the same honest way -- with a wrapper that sets the variable -- rather
# than by rewriting a linked path behind anyone's back:
#
#   the agent  carries an rpath of $DEFAULT_PREFIX/lib/r-deskvint-irix, which is
#              not where its libgcc_s.so.1 ended up
#   the panel  searches beside argv[0] and then the default prefix for the
#              helper, and finds neither. RD_HELPER overrides that search.
if [ "$PREFIX" != "$DEFAULT_PREFIX" ]; then
	mv "$BIN/r-deskvint-irix" "$LIB/r-deskvint-irix" || exit 1
	cat > "$BIN/r-deskvint-irix" <<EOF
#!/bin/sh
# Written by install.sh for a non-default prefix.
LD_LIBRARYN32_PATH="$LIB:\${LD_LIBRARYN32_PATH:-}"
export LD_LIBRARYN32_PATH
exec "$LIB/r-deskvint-irix" "\$@"
EOF
	chmod 755 "$BIN/r-deskvint-irix"

	mv "$BIN/r-deskvint-irix-gui" "$LIB/r-deskvint-irix-gui" || exit 1
	cat > "$BIN/r-deskvint-irix-gui" <<EOF
#!/bin/sh
# Written by install.sh for a non-default prefix.
RD_HELPER="$LIB/agent-helper.sh"
RD_AGENT="$BIN/r-deskvint-irix"
export RD_HELPER RD_AGENT
exec "$LIB/r-deskvint-irix-gui" "\$@"
EOF
	chmod 755 "$BIN/r-deskvint-irix-gui"
	echo "Wrote wrappers in $BIN (non-default prefix)."
fi

echo
echo "Installed:"
ls -l "$BIN/r-deskvint-irix" "$BIN/r-deskvint-irix-gui" "$LIB/agent-helper.sh" "$LIB/libgcc_s.so.1"
echo
echo "Next:"
echo "  $BIN/r-deskvint-irix --show-id        what this machine's ID is"
echo "  $BIN/r-deskvint-irix-gui              the settings panel (needs a display)"
echo "  $LIB/agent-helper.sh start           start it now"
