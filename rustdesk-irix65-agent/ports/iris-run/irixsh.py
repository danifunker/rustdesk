#!/usr/bin/env python3
"""Run commands on the emulated IRIX box over telnet.

Exists because iris-ci's serial channel cannot deliver signals -- Ctrl-C never
reaches the guest, so one blocked command wedges the console for the rest of
the session. A real telnet tty has a working line discipline, which matters a
lot when the thing you are debugging is a client that hangs.
"""
import re, socket, sys, time

HOST, PORT = "127.0.0.1", 2324
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

class Telnet:
    def __init__(self, host=HOST, port=PORT, timeout=120):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.settimeout(timeout)
        self.buf = b""

    def _negotiate(self, data):
        """Refuse every option; we only want a dumb line-oriented tty."""
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

    def read_until(self, pattern, timeout=120):
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if rx.search(self.buf):
                return True
            self.s.settimeout(max(1, deadline - time.time()))
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return False
            self.buf += self._negotiate(chunk)
        return False

    def send(self, line):
        self.s.sendall(line.encode() + b"\r\n")

    def take(self):
        out, self.buf = self.buf, b""
        return out.decode(errors="replace")

def login(user="root", password=None, timeout=180):
    t = Telnet()
    if not t.read_until(r"login:", timeout):
        raise SystemExit("no login prompt; got:\n" + t.take())
    t.take(); t.send(user)
    # Either a password prompt or straight to a shell.
    t.read_until(r"(Password:|[#$] )", 60)
    if b"Password:" in t.buf:
        t.take(); t.send(password or "")
        if not t.read_until(r"[#$] ", 60):
            raise SystemExit("login failed:\n" + t.take())
    t.take()
    t.send("PS1='RDY> '; TERM=dumb; export TERM")
    t.read_until(r"RDY> ", 60); t.take()
    return t

def run(t, cmd, timeout=300):
    t.send(cmd)
    ok = t.read_until(r"RDY> ", timeout)
    out = t.take()
    # strip the echoed command and the trailing prompt
    lines = [l for l in out.splitlines() if l.strip() not in ("", "RDY>")]
    if lines and cmd[:30] in lines[0]:
        lines = lines[1:]
    return ("\n".join(lines), ok)

if __name__ == "__main__":
    t = login()
    for cmd in sys.argv[1:]:
        out, ok = run(t, cmd, timeout=int(__import__("os").environ.get("IRIXSH_TIMEOUT", "300")))
        print(f"$ {cmd}")
        print(out if ok else out + "\n[TIMED OUT]")
        print()
