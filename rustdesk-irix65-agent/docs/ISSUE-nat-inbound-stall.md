# Inbound port forwards stop delivering after a few dozen connections

**iris** upstream at `02c4e155`, NAT networking backend, IRIX 6.5.22m guest
(Indy, R5000). Reproduced twice on 2026-08-23/24 during a packaging session.

## What happens

After roughly twenty to forty telnet sessions through the `2324 -> guest:23`
forward, new connections to `127.0.0.1:2324` are **accepted and then silent**.
The client completes the TCP handshake and receives no bytes at all — not even
telnet option negotiation — until it gives up.

This is indistinguishable from a wedged guest, which is what makes it expensive:
the obvious next move is to start debugging IRIX.

## Why it is not the guest

All of this was true at the same time, over the serial console, while telnet was
dead:

```
# the guest is listening
netstat -an | grep '\.23 '
tcp   0   0  *.23   *.*   LISTEN

# inetd is running, and had just been restarted
ps -e | grep inetd
   2658 ?   0:00 inetd

# telnet is enabled
grep '^telnet' /etc/inetd.conf
telnet  stream  tcp   nowait  root  /usr/etc/telnetd  telnetd
telnet  stream  tcp6  nowait  root  /usr/etc/telnetd  telnetd

# nothing is holding a pty -- only the serial console is logged in
who | wc -l
      1
ps -e | grep -E 'telnetd|login' | wc -l
      0

# SYSLOG has no complaint. No "service failing (looping)", nothing at all
# after the last successful login.
tail -12 /var/adm/SYSLOG

# and the guest can still reach the host, so networking is up
/usr/nekoware/bin/wget -q -O /dev/null http://192.168.0.1:8099/fetch.sh; echo $?
0
```

Outbound works, inbound does not, and the guest has no idea anything is wrong.
On the host, `ss -lntp` shows iris still owning the listener:

```
LISTEN 127.0.0.1:2324  users:(("iris",pid=2306175,fd=9))
```

## What "fixes" it, and what that suggests

- Killing inetd on the guest and starting it again brought telnet back **once**,
  within seconds. The same thing did nothing the next time.
- A guest reboot has brought it back every time (recorded across several earlier
  sessions).
- Restarting iris would presumably also do it; not tried, because a running
  guest is expensive here.

That inetd restart sometimes helps is what kept this misdiagnosed as an IRIX
inetd rate-limit for two sessions. It is a poor fit: inetd's own throttle logs
`service failing (looping)` and re-enables after ten minutes, and neither
happened.

The pattern that does fit is **per-connection state on the host side that is
allocated and not released**. Every telnet session leaves one connection; after
enough of them, new ones are accepted by the host listener but never mapped
through to the guest. Killing inetd would RST the guest ends of any lingering
connections, which would free that state — and would explain both why it helps
and why it stops helping once the entries are in a state the guest will not RST.

That is a hypothesis from the outside. What is certain is the four facts above:
listener present, guest healthy, outbound fine, inbound silent.

## Reproducing

Any loop that opens and closes telnet sessions will do it. In this tree,
`ports/iris-run/gsh.py` opens one per invocation and logs out cleanly in a
`finally`, and twenty to forty invocations was enough both times.

## What we do about it

Nothing in the repository depends on telnet any more. Everything the packaging
pipeline does goes over the serial console (`iris-ci run --shell sh`), with HTTP
for bulk transfers into the guest and `iris-ci get` for the way out — the two
directions that have never failed. `scripts/ci-lib.sh` has the detail.

## One test worth running next

Whether a *second* forward stalls at the same time. If `2222 -> guest:22` and
`21118 -> guest:21118` still deliver while 2324 does not, the state is
per-forward; if they all stop together, it is shared. That distinguishes a
per-listener leak from a global table and was not run here — the guest had no
sshd and no agent listening at the time.
