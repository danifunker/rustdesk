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
AGENT=${AGENT:-/usr/local/bin/rustdesk-agent}
PEER=${PEER:-/tmp/testpeer}
PATH=/usr/openwin/bin:/usr/local/bin:/usr/bin:/usr/sbin:$PATH
export PATH

restore() {
    echo "=== restoring the greeter ==="
    # By name, never by pattern: `pkill -f` would match this script too.
    pkill -x rustdesk-agent 2>/dev/null
    pkill -x xclock 2>/dev/null
    sudo pkill -x Xsun 2>/dev/null
    sleep 2
    sudo /etc/init.d/dtlogin start
}
trap 'restore; exit 130' 1 2 15

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

echo "=== testpeer: the real protocol, and it decodes what comes back ==="
chmod +x "$PEER"
"$PEER" 127.0.0.1:21118 "$PASSWORD" "$SECS"

echo "=== agent log ==="
cat /tmp/agent.log

restore
