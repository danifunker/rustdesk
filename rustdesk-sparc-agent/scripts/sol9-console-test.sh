#!/bin/sh
#
# sol9-console-test.sh -- prove the agent on the Solaris 9 console framebuffer.
#
# Runs ON THE BLADE, not on the build host, and everything has to happen inside
# one invocation: `Xsun` dies with the ssh session that started it, whatever
# nohup is told to do. So:
#
#     scp scripts/sol9-console-test.sh HOST:/tmp/
#     ssh HOST 'sh /tmp/sol9-console-test.sh PASSWORD'
#
# It takes the console away from CDE for the duration and gives it back at the
# end, including on the paths where the agent never starts.
#
# Bourne shell: no $( ), no [[ ]], no `local`. There is no timeout(1) here.
set -u

PASSWORD=${1:-sol9test}
SECS=${2:-15}
DISPLAY_NUM=${3:-:1}
# The agent next to this script first -- that is where the package puts both --
# then the places a hand install uses.
here=`dirname "$0"`
AGENT=${AGENT:-}
if [ -z "$AGENT" ]; then
    for a in "$here/rdeskvint" /opt/rdeskvint/bin/rdeskvint \
             /usr/local/bin/rdeskvint; do
        [ -x "$a" ] && { AGENT=$a; break; }
    done
fi
# testpeer is a development tool and is not in the package. Without it this
# still brings the display and the agent up and waits, which is what you want
# when the thing you are testing with is a real client.
PEER=${PEER:-/tmp/testpeer}
PATH=/usr/openwin/bin:/usr/local/bin:/usr/bin:/usr/sbin:$PATH
export PATH

restore() {
    echo "=== restoring the greeter ==="
    # By name, never by pattern: `pkill -f` would match this script too.
    pkill -x rdeskvint 2>/dev/null
    pkill -x xclock 2>/dev/null
    sudo pkill -x Xsun 2>/dev/null
    sleep 2
    sudo /etc/init.d/dtlogin start
}
trap 'restore; exit 130' 1 2 15

[ -n "$AGENT" ] && [ -x "$AGENT" ] || {
    echo "no rdeskvint found; set AGENT=/path/to/rdeskvint" >&2; exit 1; }

echo "=== stopping dtlogin: dtgreet holds a server grab that hangs XOpenDisplay ==="
sudo /etc/init.d/dtlogin stop
sleep 3

echo "=== Xsun on the real framebuffer (Solaris 9 has no working Xvfb) ==="
sudo /usr/openwin/bin/Xsun "$DISPLAY_NUM" -ac -nobanner -dev /dev/fbs/jfb0 defdepth 24 &
sleep 8
DISPLAY=$DISPLAY_NUM
export DISPLAY

if xdpyinfo > /tmp/xdpyinfo.txt 2>&1; then
    egrep 'dimensions|depth of root' /tmp/xdpyinfo.txt
else
    echo "Xsun did not come up; see /tmp/xdpyinfo.txt" >&2
    restore
    exit 1
fi

# A known colour, because a red/blue swap is invisible on black-and-white
# content -- it is symmetric under the swap.
echo "=== something identifiable on screen ==="
xsetroot -solid darkblue
xclock -bg orange -fg black -geometry 300x300+80+80 &
sleep 2

echo "=== agent ==="
"$AGENT" --log info > /tmp/agent.log 2>&1 &
sleep 6

if [ -x "$PEER" ]; then
    echo "=== testpeer: the real protocol, and it decodes what comes back ==="
    "$PEER" 127.0.0.1:21118 "$PASSWORD" "$SECS"
else
    echo "=== no testpeer here -- connect a real client now ==="
    echo "    address : `hostname` port 21118"
    echo "    password: the one you set with --password"
    echo "    waiting $SECS seconds, then putting the console back."
    sleep "$SECS"
fi

echo "=== agent log ==="
cat /tmp/agent.log

restore
