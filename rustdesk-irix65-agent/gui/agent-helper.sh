#!/bin/sh
#
# The non-interactive half of the IRIX settings panel.
#
# `gui_motif.c` is the window; this is everything that decides anything. The
# split is copied from the Mac bundle's deploy/agent-helper.sh, and its reason
# holds here too: the interface is the part that can only be exercised by a
# person sitting at the machine, so it should contain as little behaviour as
# possible. Everything below runs from a terminal, which is how it gets tested.
#
# It is also what keeps the panel honest about the CLI. Every `set` verb is one
# `r-deskvint-irix --flag` invocation, so the panel cannot drift into having its
# own idea of what a setting means, and anything it can do can be done without
# it.
#
# Bourne shell, not bash: /bin/sh on IRIX 6.5 is the SVR4 shell.
set -u

# Where the agent is, in the order it is likely to be there: what the caller
# said, then where the package puts it, then a SGUG-RSE tree, then the
# development /tmp. The packaged location has to come before /tmp or an
# installed machine that once had a copy in /tmp keeps using the stale one --
# which is exactly what the first install test found, with `status` reporting
# `agent=/tmp/r-deskvint-irix` on a machine that had just installed the package.
#
# `$HERE/r-deskvint-irix` comes first because it is the only entry that works for
# an install under a prefix nobody chose in advance: install.sh -p puts the real
# binary beside this script, and looking next to itself needs no configuration
# and no wrapper.
HERE=`dirname "$0"`
AGENT="${RD_AGENT:-}"
if [ -z "$AGENT" ]; then
    for _a in "$HERE/r-deskvint-irix" /usr/local/sbin/r-deskvint-irix \
              /usr/sbin/r-deskvint-irix \
              /usr/sgug/bin/r-deskvint-irix /tmp/r-deskvint-irix; do
        if [ -x "$_a" ]; then AGENT="$_a"; break; fi
    done
    [ -n "$AGENT" ] || AGENT=/usr/sbin/r-deskvint-irix
fi
# ALL OF THIS MACHINE'S SETTINGS ARE IN ONE FILE, /etc/r-deskvint-irix.conf:
# its identity (id, uuid, keys), its password, and every server setting. The
# agent uses that path itself on IRIX (config.rs SYSTEM_PATH); it is passed
# explicitly below as well, so RD_CONF can point a test somewhere else.
#
# Only root can read it or change it, deliberately. It holds the connection
# password and the private key, and it is the machine's configuration, not a
# user's. So every verb that reads or changes a setting, or starts and stops
# the agent, wants root; the panel run by anyone else shows that it is running
# and says who can change it.
#
# Until 2026-09-18 it was $HOME/.rustdesk-ppc-agent.conf, and that is how the
# boot start came to fail on its first real test: rc2 runs this with no HOME,
# `set -u` stopped the script on the line that named the file, and nothing
# started. A per-user file has no right answer at boot anyway -- and an agent
# looking in the wrong home comes up as a different machine.
CONF="${RD_CONF:-/etc/r-deskvint-irix.conf}"
LEGACY_NAME=.rustdesk-ppc-agent.conf
LOG="${RD_LOG:-/tmp/agent.log}"
PORT="${RD_PORT:-21118}"

# libgcc_s.so.1 is the one library the agent needs that IRIX does not ship.
# An INSTALLED agent finds it through its own rpath and needs nothing here; this
# is for the two cases that do not: a build run out of /tmp on a machine with
# SGUG-RSE, and an install under a prefix the rpath does not name, where the
# copy sits beside this script.
#
# /usr/sbin is still searched after /usr/local/sbin above: the package installed
# there until 2026-09-17, and this helper should keep working on a machine that
# has not been reinstalled since.
LD_LIBRARYN32_PATH="${LD_LIBRARYN32_PATH:-$HERE:/usr/sgug/lib32}"
export LD_LIBRARYN32_PATH

is_root() {
    id | grep '^uid=0(' > /dev/null
}

# need_root WHAT -- stop with a sentence the panel can show in its status line.
need_root() {
    is_root && return 0
    echo "Only root can $1: this machine's settings are in $CONF, which only root can read or change."
    exit 1
}

# The agent, always told which file. Its own default on IRIX is the same path.
agent() {
    "$AGENT" --config "$CONF" "$@"
}

# migrate_conf -- move root's old per-user file into $CONF, once.
#
# The agent does this itself when it can see a HOME. At boot it cannot -- rc2
# gives it none -- and the first thing to run after an upgrade is quite likely
# to be the boot start, which would otherwise find no $CONF, no old file, and
# generate a brand new identity: a different machine, and one hbbs will not
# let take over the old ID. So root's home comes from the password file, not
# from the environment. The old file is copied, not moved: it is somebody's
# only other copy of a signing key, and it does no harm where it is.
migrate_conf() {
    [ -f "$CONF" ] && return 0
    is_root || return 0
    _rh=`awk -F: '$3 == 0 { print $6; exit }' /etc/passwd`
    # Root's own home first: `su` without `-` keeps the caller's HOME, and
    # root must not adopt some other account's identity as the machine's.
    for _h in "$_rh" "${HOME:-}" /; do
        [ -n "$_h" ] || continue
        _old="$_h/$LEGACY_NAME"
        [ "$_h" = / ] && _old="/$LEGACY_NAME"
        [ -f "$_old" ] || continue
        cp "$_old" "$CONF.tmp" && chmod 600 "$CONF.tmp" && chown root "$CONF.tmp" &&
            mv "$CONF.tmp" "$CONF" || { rm -f "$CONF.tmp"; return 1; }
        echo "`date`: settings moved from $_old to $CONF (the old file is left where it was)" >> "$LOG"
        return 0
    done
    return 0
}

conf_get() {
    [ -r "$CONF" ] || return 0
    sed -n "s/^$1 *= *//p" "$CONF" | head -1
}

# Two settings have been renamed in the agent -- rendezvous_server -> id_server
# and server_key -> key (config.rs RENAMED). It migrates on load and rewrites
# under the new names, so reading only the old ones reports every configured
# machine as unconfigured: the panel opened with ID server and Server key blank,
# and Apply on a blank field is --no-server, which turns registration off. Read
# the new name, fall back to the old for a file nothing has rewritten yet.
conf_get_renamed() {
    _v=`conf_get "$1"`
    [ -n "$_v" ] || _v=`conf_get "$2"`
    echo "$_v"
}

# Which processes are the agent. Two IRIX traps and one of our own making, all
# of which produced a panel whose buttons looked broken:
#
#   `ps -e` truncates COMD to EIGHT characters, so `r-deskvint-irix` appears as
#   `rustdesk-` and `ps -e | grep r-deskvint-irix` matches NOTHING. Written that
#   way, is_running answered "no" for a running agent: Stop killed nothing and
#   still said "Stopped.", and Start said "Could not start it" about an agent it
#   had just started.
#
#   `ps -e -o pid,args | grep r-deskvint-irix` fixes that and matches too much.
#   It matches `r-deskvint-irix-gui` -- so Stop takes down the window that
#   pressed it -- and, once INSTALLED, it matches this script's own shell,
#   because the path is /usr/lib/RUSTDESK-AGENT/agent-helper.sh. That one is
#   invisible until the software is installed: is_running then always says yes,
#   so Start is greyed out for ever and Stop kills the helper mid-run.
#
# So: match the BASENAME OF argv[0] exactly. A process is the agent if it is the
# agent, not if its command line mentions it.
agent_pids() {
    ps -e -o pid,args | awk '
        {
            cmd = $2
            sub(/.*\//, "", cmd)
            if (cmd == "r-deskvint-irix")
                print $1
        }'
}

is_running() {
    [ -n "`agent_pids`" ]
}

# Start it detached and NOT owned by this shell's terminal. Without nohup the
# agent dies when the panel's session ends, which on a machine driven over
# telnet is every time.
start_agent() {
    ( cd /tmp && DISPLAY="${DISPLAY:-:0}" nohup "$AGENT" -v --config "$CONF" \
        --listen 0.0.0.0 --port "$PORT" >> "$LOG" 2>&1 & ) 
    sleep 3
}

stop_agent() {
    for p in `agent_pids`; do kill -9 "$p" 2>/dev/null; done
    sleep 1
}

# Before any verb, so that whichever runs first after an upgrade -- the boot
# start, the panel, a person at a shell -- finds the settings where they now
# live. A no-op once $CONF exists, and for anyone but root.
migrate_conf || echo "Could not move the old settings into $CONF; see $LOG." >&2

case "${1:-}" in

# Everything the panel shows in one call, one `key=value` per line, so the
# window makes a single round trip when it opens or refreshes rather than one
# per field.
status)
    echo "agent=$AGENT"
    echo "present=`[ -x \"$AGENT\" ] && echo yes || echo no`"
    echo "running=`is_running && echo yes || echo no`"
    # For anyone but root the file cannot be read, and blank fields would look
    # like an unconfigured machine. Say whose settings they are instead.
    if [ ! -r "$CONF" ] && [ -f "$CONF" ]; then
        echo "id=(only root can see this machine's settings)"
    else
        echo "id=`conf_get id`"
    fi
    echo "password=`conf_get password`"
    echo "server=`conf_get_renamed id_server rendezvous_server`"
    echo "relay=`conf_get relay_server`"
    echo "key=`conf_get_renamed key server_key`"
    echo "api=`conf_get api_server`"
    echo "ca=`conf_get ca_bundle`"
    echo "port=$PORT"
    # Reported rather than assumed: an agent that came up on the ReadDisplay
    # fallback works and is slow enough to look broken, and this is the only
    # place a person would find that out. See RESUME's note on the capture path.
    echo "capture=`sed -n 's/.*capture path: //p' \"$LOG\" 2>/dev/null | tail -1`"
    ;;

showkey)  need_root "read the settings"; agent --show-key 2>/dev/null || echo "(no key yet)" ;;
showid)   need_root "read the settings"; agent --show-id  2>/dev/null || echo "(no id yet)"  ;;
showlog)  tail -30 "$LOG" 2>/dev/null || echo "No log yet - the agent has not run." ;;

# Refuses to start a second one. Two agents both bind :21118, both register the
# same id, and the one that answers a peer is a coin toss -- and `stop` then
# kills both, so it looks like the first start never worked. Found by running
# the init script by hand on a machine where boot had already started one.
start)    need_root "start the agent"
          if is_running; then
              echo "Already running."
          else
              start_agent
              is_running && echo "Started." || echo "Could not start it. Try Show the log."
          fi ;;
stop)     need_root "stop the agent"; stop_agent;  is_running && echo "It is still running." || echo "Stopped." ;;
restart)  need_root "restart the agent"; stop_agent; start_agent
          is_running && echo "Restarted." || echo "Could not start it. Try Show the log." ;;

# set FIELD VALUE. An empty value is meaningful for most of these -- it is how
# a setting is cleared -- so it is passed through rather than rejected.
set)
    need_root "change a setting"
    field="${2:-}"
    value="${3:-}"
    case "$field" in
    password) out=`agent --password "$value" 2>&1`;      rc=$? ;;
    server)   if [ -n "$value" ]; then out=`agent --server "$value" 2>&1`; rc=$?
              else out=`agent --no-server 2>&1`; rc=$?; fi ;;
    relay)    out=`agent --relay-server "$value" 2>&1`;  rc=$? ;;
    key)      out=`agent --key "$value" 2>&1`;           rc=$? ;;
    api)      if [ -n "$value" ]; then out=`agent --api-server "$value" 2>&1`; rc=$?
              else out=`agent --no-api-server 2>&1`; rc=$?; fi ;;
    ca)       out=`agent --ca-bundle "$value" 2>&1`;     rc=$? ;;
    *)        echo "unknown field: $field"; exit 2 ;;
    esac

    if [ $rc -ne 0 ]; then
        echo "$out"
        exit $rc
    fi

    # Say what changed in the terms a person set it in, not the terms the CLI
    # answered in. The CLI is talking to a script; this is talking to whoever
    # just pressed the button.
    case "$field" in
    password) echo "Password set." ;;
    server)   [ -n "$value" ] && echo "ID server set to $value. Restart the agent for it to register." \
                              || echo "Registration turned off; this machine is reachable by IP only." ;;
    relay)    [ -n "$value" ] && echo "Relay set to $value." \
                              || echo "Relay override cleared; whichever relay the ID server names will be used." ;;
    key)      [ -n "$value" ] && echo "Server key set." || echo "Server key cleared." ;;
    api)      [ -n "$value" ] && echo "Console set to $value. This machine will appear in its device list within about 15 seconds." \
                              || echo "Console cleared; this machine will not appear in any device list." ;;
    ca)       [ -n "$value" ] && echo "CA bundle set to $value." \
                              || echo "CA bundle cleared; the usual places will be searched." ;;
    esac
    ;;

# setup -- walk every setting, showing what each one is now.
#
# The panel is the other way to do this, and a machine being set up over telnet
# or a serial console has no panel. Every answer goes back through `set` above
# rather than calling the agent directly, so this cannot drift from what the
# panel does, and each change is confirmed in the same words.
#
# Enter keeps a value, which means an unchanged setting is never written at all
# -- no --flag runs for it. `-` is how you clear one, because Enter is already
# spoken for; an empty answer cannot mean both "keep" and "erase".
setup)
    need_root "change the settings"
    [ -x "$AGENT" ] || { echo "No agent at $AGENT."; exit 1; }

    # Restore the terminal if this is interrupted while the password is being
    # typed, or the shell is left with echo off and no prompt to say why.
    trap 'stty echo 2>/dev/null; echo; echo "Cancelled; nothing further was changed."; exit 130' 1 2 3 15

    echo "Configuring R-DeskVint on `hostname` ($CONF)."
    echo
    echo "  Enter    keep the current value"
    echo "  -        clear it"
    echo
    echo "The ID server and console take a hostname or an IP; the ID server"
    echo "defaults to port 21116 unless you write host:port."
    echo

    for _f in server key relay api ca; do
        case "$_f" in
        server) _label="ID server      "; _cur=`conf_get_renamed id_server rendezvous_server`
                _hint="not set - THIS MACHINE is reachable by IP only" ;;
        key)    _label="Server key     "; _cur=`conf_get_renamed key server_key`
                _hint="not set - only needed for an hbbs started with -k" ;;
        relay)  _label="Relay override "; _cur=`conf_get relay_server`
                _hint="not set - use whichever relay the ID server names" ;;
        api)    _label="Console URL    "; _cur=`conf_get api_server`
                _hint="not set - this machine will not appear in a device list" ;;
        ca)     _label="CA bundle      "; _cur=`conf_get ca_bundle`
                _hint="not set - the usual places are searched" ;;
        esac

        if [ -n "$_cur" ]; then printf "%s [%s]: " "$_label" "$_cur"
        else                    printf "%s (%s): " "$_label" "$_hint"; fi
        read _ans

        case "$_ans" in
        "") continue ;;
        -)  _ans="" ;;
        esac
        sh "$0" set "$_f" "$_ans" || echo "  (not changed)"
    done

    # The password is asked for differently and shown never: the value in the
    # config is the live secret for every incoming connection, so it is not
    # printed back even to whoever is setting it.
    if [ -n "`conf_get password`" ]; then _state="set - Enter keeps it"
    else                                  _state="NOT SET - no peer can connect"; fi
    printf "Password       (%s): " "$_state"
    stty -echo 2>/dev/null
    read _ans
    stty echo 2>/dev/null
    echo
    case "$_ans" in
    "") : ;;
    -)  sh "$0" set password "" ;;
    *)  sh "$0" set password "$_ans" ;;
    esac

    trap - 1 2 3 15

    echo
    echo "Now:"
    # egrep, not sed: IRIX sed is a BRE sed with no \| alternation, and a
    # pattern using it matches nothing at all rather than failing.
    sh "$0" status | egrep '^(id|server|relay|key|api|ca)=' | sed 's/^/  /'
    echo

    # Settings are read at start-up, so a running agent is still using the old
    # ones. Saying so is the difference between this working and looking broken.
    if is_running; then
        printf "The agent is running with the OLD settings. Restart it now? [y/N]: "
        read _ans
        case "$_ans" in
        y|Y|yes|YES) stop_agent; start_agent
                     is_running && echo "Restarted." || echo "Could not start it. Try showlog." ;;
        *)           echo "Left running. Restart it with: $0 restart" ;;
        esac
    else
        printf "The agent is not running. Start it now? [y/N]: "
        read _ans
        case "$_ans" in
        y|Y|yes|YES) start_agent
                     is_running && echo "Started." || echo "Could not start it. Try showlog." ;;
        *)           echo "Not started. Start it with: $0 start" ;;
        esac
    fi
    ;;

# service install|remove|status -- boot integration, the IRIX way.
#
# /etc/init.d holds the script, rc2.d and rc0.d hold links to it, and
# /etc/config holds a flag chkconfig(1M) reads. Four pieces, which is why this
# is a verb rather than a paragraph in a README that someone half-follows.
#
# The flag is registered OFF. Installing software that silently starts
# listening on every boot is not ours to decide; `chkconfig r_deskvint_irix on`
# is one command and it is the admin's.
service)
    INIT=/etc/init.d/r_deskvint_irix
    SRC="$HERE/r_deskvint_irix.init"
    case "${2:-status}" in
    install)
        id | grep -q 'uid=0' || { echo "service install: run this as root." >&2; exit 1; }
        [ -f "$SRC" ] || { echo "service install: no $SRC" >&2; exit 1; }
        [ -d /etc/init.d ] || { echo "service install: no /etc/init.d on this machine" >&2; exit 1; }
        cp "$SRC" "$INIT" && chmod 755 "$INIT" || exit 1
        # S99 because xdm is S98, K01 because xdm is K02: up after the display,
        # down before it.
        rm -f /etc/rc2.d/S99r_deskvint_irix /etc/rc0.d/K01r_deskvint_irix
        ln -s ../init.d/r_deskvint_irix /etc/rc2.d/S99r_deskvint_irix
        ln -s ../init.d/r_deskvint_irix /etc/rc0.d/K01r_deskvint_irix
        if [ ! -f /etc/config/r_deskvint_irix ]; then
            chkconfig -f r_deskvint_irix off 2>/dev/null || echo off > /etc/config/r_deskvint_irix
        fi
        echo "Installed $INIT, and the rc links."
        echo "It is registered but OFF. To start it at every boot:"
        echo "    chkconfig r_deskvint_irix on"
        echo "and to start it now without rebooting:"
        echo "    $INIT start"
        ;;
    remove)
        id | grep -q 'uid=0' || { echo "service remove: run this as root." >&2; exit 1; }
        rm -f /etc/rc2.d/S99r_deskvint_irix /etc/rc0.d/K01r_deskvint_irix "$INIT"
        # The flag is left: chkconfig has no unregister, and a stray `off` entry
        # is inert. Removing the file by hand is what clears it from the list.
        echo "Removed $INIT and the rc links."
        [ -f /etc/config/r_deskvint_irix ] &&
            echo "The chkconfig flag is left behind; rm /etc/config/r_deskvint_irix clears it."
        ;;
    status)
        [ -f "$INIT" ] && echo "init script : $INIT" || echo "init script : not installed"
        [ -h /etc/rc2.d/S99r_deskvint_irix ] && echo "start link  : /etc/rc2.d/S99r_deskvint_irix" \
                                            || echo "start link  : missing"
        [ -h /etc/rc0.d/K01r_deskvint_irix ] && echo "stop link   : /etc/rc0.d/K01r_deskvint_irix" \
                                            || echo "stop link   : missing"
        if [ -f /etc/config/r_deskvint_irix ]; then
            echo "chkconfig   : `cat /etc/config/r_deskvint_irix`"
        else
            echo "chkconfig   : not registered"
        fi
        echo "running now : `is_running && echo yes || echo no`"
        ;;
    *)
        echo "usage: $0 service install|remove|status"
        exit 2
        ;;
    esac
    ;;

*)
    echo "usage: $0 status | showid | showkey | showlog | start | stop | restart"
    echo "       $0 set password|server|relay|key|api|ca VALUE"
    echo "       $0 setup      ask for each setting in turn"
    echo "       $0 service install|remove|status   start it at boot (chkconfig)"
    exit 2
    ;;
esac
