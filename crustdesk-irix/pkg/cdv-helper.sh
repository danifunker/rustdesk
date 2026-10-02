#! /bin/sh
#
# What the settings panel (cdeskvint-panel) does, one verb at a time. Plain
# Bourne shell, testable from a terminal; the panel only runs it and shows
# what it says. As root:
#
#   cdv-helper.sh status          key=value lines: present, running, boot, id,
#                                 password, server, key, relay, api, capture
#   cdv-helper.sh set FIELD VALUE password, server, key, relay, api or capture;
#                                 a running agent is restarted to use it
#   cdv-helper.sh start | stop | restart
#   cdv-helper.sh boot on | off   start it at boot or not (chkconfig cdeskvint)
#   cdv-helper.sh showid          the ID and password, as C-Desk-Vint -i says
#   cdv-helper.sh showlog         the end of the agent's log
#
# Written for IRIX 5.3's sh: backquotes, no $(...), no local.

AGENT=/usr/local/sbin/C-Desk-Vint
DIR=/var/C-Desk-Vint
PREFS=$DIR/prefs
LOG=$DIR/log
HOME=$DIR
export HOME

is_root() {
	case "`/usr/bin/id`" in
	uid=0\(*) return 0 ;;
	esac
	return 1
}

need_root() {
	is_root && return 0
	echo "Only root can change C-Desk-Vint: log in as root, or run the panel as root."
	exit 1
}

running() {
	/bin/ps -ef | grep 'C-Desk-Vint -D' | grep -v grep > /dev/null
}

# One setting from the prefs file ("" if it is not there).
pref() {
	[ -r $PREFS ] || return 0
	sed -n "s/^$1=//p" $PREFS | sed -n '$p'
}

# The agent makes its identity (ID, password, key) the first time it runs.
ensure_prefs() {
	[ -f $PREFS ] && return 0
	[ -d $DIR ] || mkdir $DIR
	chmod 700 $DIR
	$AGENT -i -c $PREFS > /dev/null 2>&1
}

start_agent() {
	if running; then
		echo "The agent is already running."
		return 0
	fi
	[ -d $DIR ] || mkdir $DIR
	chmod 700 $DIR
	DISPLAY=:0 $AGENT -D -c $PREFS -l $LOG
	sleep 2
	if running; then
		echo "The agent is running."
	else
		echo "The agent did not start: see the log (Agent > Show the Log)."
		return 1
	fi
}

stop_agent() {
	if running; then :; else
		echo "The agent is not running."
		return 0
	fi
	/sbin/killall -TERM C-Desk-Vint
	n=0
	while running && [ $n -lt 10 ]; do
		sleep 1
		n=`expr $n + 1`
	done
	if running; then
		echo "The agent did not stop."
		return 1
	fi
	echo "The agent is stopped."
}

case "$1" in
status)
	if [ -x $AGENT ]; then echo present=yes; else echo present=no; fi
	if running; then echo running=yes; else echo running=no; fi
	if /sbin/chkconfig cdeskvint; then echo boot=on; else echo boot=off; fi
	if is_root; then
		ensure_prefs
		echo "id=`pref id`"
		echo "password=`pref password`"
		echo "server=`pref server`"
		echo "key=`pref key`"
		echo "relay=`pref relay`"
		echo "api=`pref api`"
		c=`pref capture`
		echo "capture=${c:-auto}"
	else
		echo "id=(only root can see it)"
	fi
	;;
set)
	need_root
	field=$2
	value=$3
	case "$field" in
	password|server|key|relay|api|capture) ;;
	*) echo "There is no setting called $field."; exit 1 ;;
	esac
	case "$value" in
	*"
"*) echo "One line only, please."; exit 1 ;;
	esac
	case "$field=$value" in
	password=) echo "The password cannot be empty."; exit 1 ;;
	capture=|capture=auto|capture=8|capture=24|capture=readdisplay) ;;
	capture=*) echo "Capture is one of auto, 8, 24 or readdisplay."; exit 1 ;;
	server=*" "*|relay=*" "*|api=*" "*|key=*" "*) echo "No spaces in a $field."; exit 1 ;;
	esac
	ensure_prefs
	cur=`pref $field`
	if [ $field = capture ]; then
		cur=${cur:-auto}
		value=${value:-auto}
	fi
	if [ "$cur" = "$value" ]; then
		echo "That is already the $field."
		exit 0
	fi
	egrep -v "^$field=" $PREFS > $PREFS.new
	echo "$field=$value" >> $PREFS.new
	chmod 600 $PREFS.new
	mv $PREFS.new $PREFS
	if running; then
		stop_agent > /dev/null
		start_agent > /dev/null
		echo "Saved the $field, and restarted the agent to use it."
	else
		echo "Saved the $field. It takes effect when the agent starts."
	fi
	;;
start)
	need_root
	start_agent
	;;
stop)
	need_root
	stop_agent
	;;
restart)
	need_root
	stop_agent > /dev/null
	start_agent
	;;
boot)
	need_root
	case "$2" in
	on)  /sbin/chkconfig -f cdeskvint on;  echo "The agent will start at every boot." ;;
	off) /sbin/chkconfig -f cdeskvint off; echo "The agent will not start at boot." ;;
	*)   echo "boot on, or boot off."; exit 1 ;;
	esac
	;;
showid)
	need_root
	ensure_prefs
	$AGENT -i -c $PREFS
	;;
showlog)
	need_root
	if [ -r $LOG ]; then tail -150 $LOG; else echo "There is no log yet."; fi
	;;
*)
	echo "usage: cdv-helper.sh status | set FIELD VALUE | start | stop | restart"
	echo "       | boot on|off | showid | showlog"
	exit 2
	;;
esac
