#!/usr/bin/env python3
"""The terminal and a tunnel on a real NT machine, by its direct port.

    session-check.py HOST[:PORT]

Asks for the agent's password at a hidden prompt. Disconnect any session from
the machine first if it would be the terminal's; someone must be logged on
there, since the terminal's shell runs in the tray. The tunnel goes to the
machine's own FTP server (port 21): its greeting comes back through it, and
the command QUIT shows that bytes go both ways.
"""
import getpass
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                "crustdesk-macos-classic", "host"))
from cdvpoke import Peer, field, parse  # noqa: E402


def response(p, want, timeout):
    """The next TerminalResponse of kind `want`, or None."""
    end = time.time() + timeout
    while time.time() < end:
        m = p.recv(max(0.05, end - time.time()))
        if m and 32 in m:
            r = parse(m[32][0])
            if want in r:
                return parse(r[want][0])
    return None


def output(p, seconds):
    text = b""
    end = time.time() + seconds
    while time.time() < end:
        d = response(p, 2, max(0.05, end - time.time()))
        if d:
            text += d[2][0]
    return text


def terminal(addr, password):
    p = Peer(addr)
    p.login_extra = field(16, 2, field(1, 2, b""))
    p.login(password)
    print("features:", parse(p.peer_info[9][0]) if 9 in p.peer_info else "none")
    p.send(field(31, 2, field(1, 2, field(1, 0, 1) + field(2, 0, 25) + field(3, 0, 80))))
    o = response(p, 1, 10)
    if not o or o.get(2) != [1]:
        raise SystemExit("terminal did not open: %r" % o)
    print("opened: pid %s, service %s" % (o.get(4), o.get(5)))
    print("first output:", repr(output(p, 3))[-200:])
    p.send(field(31, 2, field(2, 2, field(1, 0, 1) + field(2, 2, b"ver\r"))))
    text = output(p, 4)
    print("after ver:", repr(text)[-200:])
    if b"Windows NT" not in text:
        raise SystemExit("no answer from cmd")
    p.s.close()
    print("terminal: ok")


def tunnel(addr, password):
    p = Peer(addr)
    p.login_extra = field(8, 2, field(1, 2, b"localhost") + field(2, 0, 21))
    p.login(password)
    p.s.settimeout(10)
    hello = p.buf or p.s.recv(4096)
    print("through the tunnel:", repr(hello)[:100])
    if not hello.startswith(b"220"):
        raise SystemExit("no FTP greeting")
    p.s.sendall(b"QUIT\r\n")
    bye = p.s.recv(4096)
    print("after QUIT:", repr(bye)[:100])
    if not bye.startswith(b"221"):
        raise SystemExit("no answer to QUIT")
    p.s.close()
    print("tunnel: ok")


def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "192.168.99.118:21118"
    if ":" not in addr:
        addr += ":21118"
    password = getpass.getpass("Agent password (hidden): ")
    terminal(addr, password)
    time.sleep(1)
    tunnel(addr, password)
    print("all passed")


if __name__ == "__main__":
    main()
