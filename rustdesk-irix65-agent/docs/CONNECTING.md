# Connecting a real RustDesk client to the emulated Indy

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
`/root/.rustdesk-ppc-agent.conf`** (`hunter2` on this image).

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
