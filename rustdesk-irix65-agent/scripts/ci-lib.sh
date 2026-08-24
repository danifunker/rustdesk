# scripts/ci-lib.sh -- helpers shared by the build and packaging scripts, so a
# local run and a CI run go through the SAME code. Sourced, never executed; the
# caller sets $REPO first and keeps its own die(), so an error message names the
# script the person actually ran.
#
# The same split as ../irixscsitb's ci-lib.sh, and for the same reason: the one
# thing a pipeline must not have is a local path that only works on the machine
# it was written on.

CONF="${REPO:?ci-lib.sh: caller must set REPO}/ci/local.conf"

# conf_get KEY -- one value out of ci/local.conf (per-machine, .gitignore'd).
# Parsed as KEY=VALUE and deliberately NEVER sourced: no shell code runs from a
# config file. Surrounding double quotes are stripped.
conf_get() {
	[ -f "$CONF" ] || return 0
	sed -n "s/^$1=//p" "$CONF" | head -1 | sed 's/^"//; s/"$//'
}

# load_local_conf -- pull the recognised keys out of ci/local.conf into the
# environment WITHOUT overriding anything already set, so a command-line flag or
# a real environment variable always beats the file. Call once, after argument
# parsing.
load_local_conf() {
	for _k in IRIX_SYSROOT SGUG_STAGING PPC_AGENT_DIR \
	          IRIX65_IMAGE IRIX65_DISK_URL \
	          IRIS_DIR IRIS_SOCKET IRIS_RELEASE_REPO IRIS_TAG; do
		_cur=$(eval "printf %s \"\${$_k:-}\"")
		[ -n "$_cur" ] && continue
		_v=$(conf_get "$_k")
		[ -n "$_v" ] || continue
		eval "$_k=\$_v"
		export "$_k"
	done
}

# The image and emulator keys are spelled exactly as ../irixscsitb spells them,
# on purpose: the same private disk image and the same GitHub secret serve both
# repositories, and a person who has configured one has configured the other.
#   IRIX65_IMAGE       a local .chd path (the dispatch input arrives this way)
#   IRIX65_DISK_URL    a private download URL (a repo secret in Actions)
#   IRIS_DIR           a checkout/build of iris, if you have one
#   IRIS_RELEASE_REPO  where fetch-iris.sh looks for prebuilt binaries
#   IRIS_TAG           pin a release; blank means latest

# resolve_disk_url -- the private download URL for the boot image, if set.
resolve_disk_url() {
	printf %s "${IRIX65_DISK_URL:-}"
}

# resolve_local_image -- a local boot image path, if one is configured.
resolve_local_image() {
	printf %s "${IRIX65_IMAGE:-}"
}

# version_string -- what goes in artifact filenames.
#
# A date plus the short revision: the date is what a person reads off a download
# and the revision is what a bug report needs. A dirty tree says so, because a
# binary built from uncommitted work corresponds to no commit and that is
# exactly the thing you want to know when it misbehaves.
version_string() {
	_rev=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)
	_date=$(date -u '+%Y%m%d' 2>/dev/null)
	if [ -z "$_rev" ]; then
		printf '%s-nogit' "$_date"
		return 0
	fi
	if [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" ]; then
		printf '%s-%s-dirty' "$_date" "$_rev"
	else
		printf '%s-%s' "$_date" "$_rev"
	fi
}

# dist_version_from VERSION -- the numeric version inst wants.
#
# inst compares versions numerically to decide what is an upgrade, so a version
# that starts with the date orders correctly for free. This keeps the first ten
# digits of the version string: the eight of YYYYMMDD, then whatever two digits
# fall out of the revision hash. Those two are arbitrary but deterministic --
# the same commit always gives the same number -- so they break a same-day tie
# consistently and never reorder two different days. `replaces self` in the spec
# means an equal version still installs.
dist_version_from() {
	printf %s "$1" | tr -cd '0-9' | cut -c1-10
}

# stage_inst_inputs FLAVOR DISTVER DESTDIR -- write the version-stamped,
# flavor-specific product description (spec + idb) into DESTDIR.
#
# One subsystem per product, named for the ABI. The guest that runs gendist is
# the OS that packages the build, exactly as irixscsitb does it: n32 is packaged
# by a 6.5 guest. An o32 flavor would be packaged by a 5.3 one, which is why the
# ABI is a parameter rather than a constant even though only n32 exists today.
stage_inst_inputs() {
	_fl="$1"; _dv="$2"; _dst="$3"
	case "$_fl" in
		n32) _abi="n32, IRIX 6.5" ;;
		o32) _abi="o32, IRIX 5.3-6.5" ;;
		*)   return 1 ;;
	esac
	sed -e "s/@VERSION@/$_dv/" -e "s/@SUBSYS@/$_fl/" -e "s/@ABI_DESC@/$_abi/" \
		"$REPO/inst/rustdesk-agent.spec" > "$_dst/rustdesk-agent.spec"
	sed -e "s/@SUBSYS@/$_fl/" \
		"$REPO/inst/rustdesk-agent.idb" > "$_dst/rustdesk-agent.idb"
}

# ---------------------------------------------------------------------------
# talking to a running guest
# ---------------------------------------------------------------------------

# guest_sh CMD... -- run commands on the guest over telnet, printing the output.
# Thin wrapper over ports/iris-run/gsh.py so callers do not each spell the path.
guest_sh() {
	python3 "$REPO/ports/iris-run/gsh.py" "$@"
}

# guest_run TIMEOUT CMD -- run one command on the guest over the SERIAL console.
#
# The serial console, not telnet, and that is the whole design of this pipeline.
# The telnet forward stalls after a few dozen sessions: the host-side listener
# still accepts, and nothing ever comes back -- no login prompt, no error, and
# nothing in the guest's SYSLOG. It is not inetd. When it happened during this
# packaging work the guest was listening on *.23, /etc/inetd.conf had telnet
# enabled, inetd had been restarted, and the guest could still fetch over HTTP
# from the host -- so outbound NAT was fine and inbound was not. Restarting
# inetd sometimes clears it, which is what made it look like an inetd fault for
# a while; see docs/ISSUE-nat-inbound-stall.md.
#
# The console has never once failed. It is slower and it mangles very long
# lines, so anything long goes into a script that is fetched over HTTP -- which
# works, because that direction is outbound.
#
# --shell sh because the guest's root shell is bash: iris-ci defaults to csh and
# asks for `$status`, so every command comes back "guest exit -1" and an exit
# code cannot be trusted. With sh it uses `$?` and the code is real.
guest_run() {
	_to="$1"; shift
	"${IRIS_CI_BIN:?guest_run: caller must set IRIS_CI_BIN}" \
		run --shell sh --timeout "$_to" "$*"
}

# guest_get GUESTPATH HOSTPATH -- pull a file off the guest, with a retry.
#
# `iris-ci get` works out which shell the console is running by probing
# `echo ZZSHELLZZ=$0` and choosing sh or csh syntax from the answer. That probe
# can miss on a console that has just been worked hard, and it then sends CSH
# syntax -- `>& /dev/null`, `echo IRIS-CI-RC=$status` -- to bash, which produces
# an empty exit code and a transfer that fails reporting "iris-ci get needs a
# shell on the serial console". The shell was there; the detection was not.
#
# Seen once in something like fifteen transfers, on the SECOND get of a run
# whose first one had just succeeded. One bad detection should not cost a
# twenty-minute packaging run, so: settle the console with a trivial command
# and try again.
guest_get() {
	_try=0
	while [ $_try -lt 3 ]; do
		guest_run 20 'echo GET-SETTLE' > /dev/null 2>&1 || true
		if "${IRIS_CI_BIN:?guest_get: caller must set IRIS_CI_BIN}" \
		     get "$1" --to "$2" --timeout "${3:-900}"; then
			return 0
		fi
		_try=$(expr $_try + 1)
		echo "    (transfer failed, retrying -- $_try of 3)" >&2
		sleep 3
	done
	return 1
}

# guest_login -- put a shell on the serial console.
#
# Needed before guest_run and before `iris-ci get`, both of which otherwise wait
# out their whole timeout against a login prompt having done nothing at all.
guest_login() {
	"${IRIS_CI_BIN:?guest_login: caller must set IRIS_CI_BIN}" login root > /dev/null 2>&1
}
