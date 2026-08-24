#!/bin/sh
# Pull the current build and helper scripts onto the guest.
#
# A script rather than a command line: anything long enough to name several
# files corrupts itself in the telnet tty and comes back as a bash syntax error
# on something nobody typed. See RESUME's list of mistakes not to repeat.
W=/usr/nekoware/bin/wget
H=http://192.168.0.1:8099
cd /tmp
for f in rustdesk-agent rustdesk-agent-gui xpoke testpeer perfprobe perfprobe-stock perfprobe-nocopy pipeline capture-selftest portable-selftest; do
    $W -q $H/$f -O /tmp/$f.new && mv /tmp/$f.new /tmp/$f && chmod 755 /tmp/$f
done
for f in testca.pem run-agent.sh setup-screen.sh colour-check.sh mouse-check.sh decode-check.sh ab-floor.sh fidelity-check.sh rendezvous-check.sh xdm-check.sh xdm-stop.sh quality-sweep.sh gui-run.sh gui-shot.sh gui-press.sh screen-typing.sh workload-compare.sh fps-matrix.sh cleanpty.sh agent-helper.sh fetch.sh; do
    $W -q $H/$f -O /tmp/$f.new && mv /tmp/$f.new /tmp/$f && chmod 755 /tmp/$f
done
ls -l /tmp/rustdesk-agent /tmp/testpeer /tmp/perfprobe
