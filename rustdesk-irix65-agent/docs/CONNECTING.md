# Connecting a real RustDesk client to the emulated Indy

> Two different things are described here. **Pointing the agent at your own
> rendezvous server** — an hbbs you host, reachable by ID from anywhere — is the
> section at the bottom, *Your own server*. Everything before it is the
> direct-IP loopback setup used to develop against the emulator.


Everything in `RESUME.md` was measured with `testpeer`, which runs **on the
guest** and talks to the agent over loopback. That proves the protocol and it
measures the agent honestly, and it is not the same as looking at the desktop
with your own eyes through the client you would actually use.

This is how to do that.

## Once: the port forward and the listen address

Both are already in git, but they are the two things that have to be right, and
the failure mode of each is "connection refused" with nothing in any log.

`ports/iris-run/iris.toml` forwards the agent's port:

```toml
[[port_forward]]
proto      = "tcp"
host_port  = 21118
guest_port = 21118
bind       = "localhost"
```

**A port forward is read at start-up**, so an emulator that was already running
when this was added does not have it. Restart it.

`ports/iris-run/guest/run-agent.sh` binds `0.0.0.0`, not `127.0.0.1`. iris NATs
the host's connection in, so it arrives at the guest from `192.168.0.1` rather
than from loopback; binding loopback serves a peer on the guest and nothing
else.

## Then, every time

On the host, from `ports/iris-run`:

```sh
./serve.sh &                       # the build and the guest scripts on :8099
```

On the guest:

```sh
sh /tmp/fetch.sh                   # pull the current build
sh /tmp/setup-screen.sh            # something to look at (xclock + an xterm)
sh /tmp/run-agent.sh               # starts it listening on 0.0.0.0:21118
```

The last one prints the line that matters:

```
[  0.324] INFO  agent listening on 0.0.0.0:21118 (id ff6izj02b)
[ 12.196] INFO  capture path: damage (SGI-SCREEN-CAPTURE)
```

If the capture path line says anything other than `damage`, stop: every frame
will be a full-screen read and the numbers will be meaningless. `restart-x.sh`
usually fixes it.

## In the client

Connect to **`127.0.0.1:21118`**, password **from the guest's
`/etc/r-deskvint-irix.conf`** (`hunter2` on this image). Builds before
2026-09-18 kept it in root's `~/.rustdesk-ppc-agent.conf`; the first root run of
a newer one moves it.

This is upstream's direct-IP mode: no rendezvous server, no key exchange, and
**the session is unencrypted**. That is deliberate and the agent says so in its
log. It is fine over a loopback forward and is not fine over a network you do
not own.

## Checking it is really connected

From the host, without any client at all:

```sh
python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', 21118), timeout=20)
d = s.recv(4096)
print(len(d), 'bytes:', ''.join(chr(b) if 32<=b<127 else '.' for b in d[:40]))"
```

A working agent answers immediately with its `hash` message — the salt and
challenge are ASCII and legible in the dump:

```
19 bytes: HJ...xtg9x2..yvnpnt
```

Nothing back means the forward is missing or the agent is bound to loopback.
A refused connection means the agent is not running.

**Or the forward itself has stopped delivering.** iris's inbound NAT stalls
after enough connections: the host-side listener still accepts and nothing is
ever passed to the guest, which from here looks exactly like an agent that has
gone deaf. It was found on the telnet forward, and there is no reason to think
21118 is special. `docs/ISSUE-nat-inbound-stall.md` has what was ruled out and
what brings it back. If a session that was working stops, check the guest over
the serial console (`iris-ci run --shell sh 'ps -e -o pid,args'`) before
suspecting the agent.

## What to expect

Pick the picture size with the client's **image quality** control, which the
agent now acts on: Best is 1280x1024, Balanced 640x512, Low 320x256. The agent
announces the size with a `SwitchDisplay` before the first frame in it, so the
client's canvas follows.

On the **emulator**, per §How fast is it, honestly in `RESUME.md`: expect about
3 fps at 1280x1024 and 9 fps at 640x512 for ordinary pointer-and-typing work,
and roughly a third of that while something repaints a whole window. The
emulated R5000 runs at about a third of a real Indy, so real hardware should be
three times each of those — but that ratio is inferred from the emulator's cycle
rate and **has never been checked against a real machine**, which is the single
most useful measurement nobody has taken yet.

---

## Your own server

Direct-IP above is for the emulator. To make a machine reachable by ID through
an hbbs you host, there are three settings and one command:

```sh
/usr/local/lib/r-deskvint-irix/agent-helper.sh setup
```

It asks for each one in turn, showing what it is now. **Enter** keeps a value,
**`-`** clears it, and the password is not echoed and never printed back. At the
end it offers to restart, which matters: settings are read at start-up, so a
running agent is still using the old ones until it is.

The same settings, non-interactively, or from the Motif panel's fields:

```sh
A=/usr/local/lib/r-deskvint-irix/agent-helper.sh
$A set server hbbs.example.com           # HOST or HOST:PORT, default 21116
$A set key '<server key>'                # only for an hbbs started with -k
$A set api https://console.example.com   # optional, and separate from `server`
$A set password '<chosen password>'
$A restart
```

`server` and `api` are independent: the first makes the machine **reachable** by
ID, the second makes it **visible** in a console's device list. Either works
alone.

**What has to be open, outbound from the machine:** UDP 21116 to hbbs for
registration and heartbeat, TCP 21117 to hbbr for the relay, and 443 to the
console if `api` is set. A machine behind NAT never needs anything forwarded to
it — peers arrive over the relay. Forwarding TCP 21118 to it only adds the
direct path.

**These sessions are encrypted** whichever way `--secure` is set: a peer that
arrives through the server always takes part in the key exchange. `--secure`
governs the direct-IP listener on 21118, where the client does not — so set it
if the machine is reachable from outside and you do not want that unencrypted
path available. See the comment above the registration block in `main.rs`.

Afterwards, `agent-helper.sh status` reports every setting and whether it is
running, and `showlog` is where a registration that is not working says why.
