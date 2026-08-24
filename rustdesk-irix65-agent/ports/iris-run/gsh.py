#!/usr/bin/env python3
"""Run guest commands over telnet, one marker per command so output can never
be attributed to the wrong command.

irixsh.py strips the echoed command line heuristically, which slides output
by one command whenever the guest echoes differently than expected. This
brackets every command with a unique marker instead, so slicing is exact.

Usage:  gsh.py 'cmd1' 'cmd2' ...
        gsh.py -f commands.txt      (one command per line, # comments ok)
Env:    GSH_TIMEOUT         per-command timeout in seconds (default 300)
        GSH_LOGIN_TIMEOUT   seconds to wait for the login prompt (default 240).
                            Worth turning down for a liveness probe: telnetd
                            stops answering every couple of dozen sessions
                            (inetd, not the guest -- see RESUME), and a caller
                            that only wants to know WHETHER it answers should
                            not spend four minutes finding out.
"""
import os, re, socket, sys, time

HOST, PORT = "127.0.0.1", 2324
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class Telnet:
    def __init__(self, host=HOST, port=PORT, timeout=120):
        self.s = socket.create_connection((host, port), timeout=15)
        self.s.settimeout(timeout)
        self.buf = b""

    def _negotiate(self, data):
        out, i, clean = bytearray(), 0, bytearray()
        while i < len(data):
            b = data[i]
            if b == IAC and i + 1 < len(data):
                cmd = data[i + 1]
                if cmd in (DO, DONT) and i + 2 < len(data):
                    out += bytes([IAC, WONT, data[i + 2]]); i += 3; continue
                if cmd in (WILL, WONT) and i + 2 < len(data):
                    out += bytes([IAC, DONT, data[i + 2]]); i += 3; continue
                if cmd == SB:
                    j = data.find(bytes([IAC, SE]), i)
                    i = (j + 2) if j != -1 else len(data); continue
                if cmd == IAC:
                    clean.append(IAC); i += 2; continue
                i += 2; continue
            clean.append(b); i += 1
        if out:
            self.s.sendall(bytes(out))
        return bytes(clean)

    def read_until(self, pattern, timeout=300):
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = rx.search(self.buf)
            if m:
                return m
            self.s.settimeout(max(1, deadline - time.time()))
            try:
                chunk = self.s.recv(8192)
            except socket.timeout:
                continue
            except OSError:
                return None
            if not chunk:
                return None
            self.buf += self._negotiate(chunk)
        return None

    def send(self, line):
        self.s.sendall(line.encode() + b"\r\n")


def login(user="root", password=None, timeout=None):
    if timeout is None:
        timeout = int(os.environ.get("GSH_LOGIN_TIMEOUT", "240"))
    t = Telnet()
    if not t.read_until(r"login:", timeout):
        raise SystemExit("no login prompt")
    t.send(user)
    t.read_until(r"(Password:|[#$] )", 90)
    if b"Password:" in t.buf:
        t.send(password or "")
        if not t.read_until(r"[#$] ", 90):
            raise SystemExit("login failed")
    t.buf = b""
    # A dumb terminal and no prompt-driven parsing: markers do the work.
    # A wide terminal matters: telnetd echoes the command back, and at 80
    # columns a long line wraps, which inserts spaces mid-marker and makes the
    # reply unparseable. Echo stays on because stty -echo does not survive here.
    t.send("PS1=''; TERM=dumb; export TERM; stty columns 1000 rows 200 2>/dev/null; echo READY_MARK")
    t.read_until(r"READY_MARK", 90)
    t.buf = b""
    return t


ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]|\x1b[()][A-B0-2]|\r")


def run(t, cmd, timeout=300):
    tag = "M%d_%d" % (int(time.time() * 1000) % 1000000, run.counter)
    run.counter += 1
    start, end = "S" + tag, "E" + tag
    t.buf = b""
    t.send("echo %s; %s; echo %s rc=$?" % (start, cmd, end))
    m = t.read_until(re.escape(end).encode() + rb" rc=(\d+)", timeout)
    raw = t.buf
    if m is None:
        txt = ANSI.sub(b"", raw).decode(errors="replace")
        i = txt.rfind(start)
        if i >= 0:
            txt = txt[i + len(start):]
        return txt.strip(), None
    rc = int(m.group(1))
    txt = ANSI.sub(b"", raw).decode(errors="replace")
    # The guest echoes the command line back, so the marker appears twice: once
    # inside the echo and once as real output. Take the LAST start marker and
    # the first end marker after it, or the echoed line is mistaken for output.
    i = txt.rfind(start)
    body = txt[i + len(start):] if i >= 0 else txt
    j = body.find(end)
    if j >= 0:
        body = body[:j]
    return body.strip("\n"), rc


run.counter = 0


def main():
    args = sys.argv[1:]
    cmds = []
    if args and args[0] == "-f":
        for line in open(args[1]):
            line = line.strip()
            if line and not line.startswith("#"):
                cmds.append(line)
    else:
        cmds = args
    timeout = int(os.environ.get("GSH_TIMEOUT", "300"))
    t = login()
    try:
        for c in cmds:
            out, rc = run(t, c, timeout)
            print("$ " + c)
            if out:
                print(out)
            print("[rc=%s]" % ("TIMEOUT" if rc is None else rc))
            print()
            sys.stdout.flush()
    finally:
        # Log out, always.
        #
        # Dropping the socket without this leaves the guest's `login` and
        # `telnetd` behind holding a pty, and IRIX has few of them. After a few
        # dozen runs telnetd accepts the connection, completes the option
        # negotiation, and then never prints a login prompt -- which is
        # indistinguishable from a wedged guest and cost half an hour to
        # recognise. `who` on the serial console is what shows it.
        try:
            t.send("exit")
            time.sleep(0.3)
        except Exception:
            pass
        try:
            t.s.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
