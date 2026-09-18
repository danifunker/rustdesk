#!/bin/sh
# Run the agent on whatever CPU this guest has, and say what happened.
#
# Written for the first run on MIPS III. The agent is built -march=mips3, but
# until 2026-09-18 it had only ever run on an R5000 (the emulator) and an R10000
# (the O2) -- both MIPS IV. An instruction MIPS III lacks would pass on both and
# kill the agent with SIGILL on an R4400 Indy. Boot the guest with
# `scripts/iris-guest.sh start --cpu r4400 --graphics` and run this.
#
# Expects in /tmp, fetched over HTTP by the caller:
#   r-deskvint-irix      the agent
#   agent-helper.sh      the helper
#   rdlib/libgcc_s.so.1  the one library the package ships; the agent's rpath
#                        names the installed location, which a bare guest lacks
#
# Every step prints its exit status. 132 is 128 + SIGILL (4): that is the
# number this script exists to look for. Nothing here stops on a failure --
# the point is to see all of it.
unset RD_HELPER
LD_LIBRARYN32_PATH=/tmp/rdlib
export LD_LIBRARYN32_PATH
A=/tmp/r-deskvint-irix
TMO=/root/tmo

echo "=== the machine ==="
uname -a
hinv -c processor

echo "=== --help ==="
$A --help > /tmp/m3-help.out 2>&1
echo "rc=$?  (`wc -l < /tmp/m3-help.out` lines)"

echo "=== --show-id ==="
$A --show-id
echo "rc=$?"

echo "=== agent-helper.sh status ==="
RD_AGENT=$A sh /tmp/agent-helper.sh status
echo "rc=$?"

echo "=== the X server ==="
# Bounded: a server that is not answering stalls a client rather than refusing
# it, and xdpyinfo has no timeout of its own.
#
# Judged by what xdpyinfo PRINTS, never by an exit status: /root/tmo exits 0
# whether the command finished or was killed, and says only
# "[TMO: killed after Ns]". The first run of this script read that 0 as "X
# answers" -- at an xdm greeter, which answers nobody. xdm starts at boot on
# this image; the bare server restart-x.sh starts is what every emulator number
# in RESUME.md was measured against.
x_answers() {
	$TMO 25 /usr/bin/X11/xdpyinfo -display :0 2>&1 | grep -q 'name of display'
}
ps -e -o pid,args | grep -E 'X11/xdm|Xsgi' | grep -v grep | cut -c1-100
if x_answers; then
	echo "X on :0 answers"
else
	echo "X on :0 did not answer (an xdm greeter holds it) -- starting a bare server"
	/root/restart-x.sh > /tmp/m3-x.out 2>&1
	tail -3 /tmp/m3-x.out
	if x_answers; then echo "X on :0 answers"; else echo "X on :0 STILL not answering"; fi
fi

echo "=== --probe-display ==="
DISPLAY=:0 $TMO 1500 $A --probe-display > /tmp/m3-probe.out 2>&1
echo "rc=$?"
cat /tmp/m3-probe.out

echo "=== rld and the kernel, if either had anything to say ==="
# A dynamic executable that dies in rld writes to SYSLOG, not stderr.
tail -40 /var/adm/SYSLOG | grep -i 'rld\|r-deskvint\|illegal\|SIGILL' | tail -8
echo MIPS3-CHECK-DONE
