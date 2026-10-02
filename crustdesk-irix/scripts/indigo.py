#!/usr/bin/env python3
"""Drive an IRIX machine over telnet: run commands, move files, link the agent.

IRIX 5.3 has no ssh, and its ftpd will not take root; telnet is what there is.
Typing files through the terminal loses characters once the pty's input queue
fills (IRIX discards rather than blocking), so the terminal carries only one
small file: tools/irix/xfer.c, compiled there with the system cc. After that
files move over a plain TCP connection to this machine -- `xfer get` connects
back and reads -- and are checked with `sum` on arrival.

    indigo.py run 'command'          run a command, print its output
    indigo.py put LOCAL REMOTE...    copy files to the machine
    indigo.py get REMOTE LOCAL       copy a file back
    indigo.py spawn SECS 'command'   start something that keeps running; print
                                     what it says in its first SECS seconds
    indigo.py link [DIR]             upload build/obj (what changed), link there,
                                     and fetch the executable to build/C-Desk-Vint

Environment: INDIGO (host, default 192.168.99.38), INDIGO_USER (root),
INDIGO_PASS (empty), INDIGO_DIR (/usr/tmp/cdv-build).
"""
import binascii, hashlib, json, os, re, sys, time, warnings
warnings.filterwarnings("ignore", category=DeprecationWarning)
import telnetlib

HOST = os.environ.get("INDIGO", "192.168.99.38")
USER = os.environ.get("INDIGO_USER", "root")
PASS = os.environ.get("INDIGO_PASS", "")
MARK = b"__IRUN_RC="
PROMPT = b"IRUN$ "


def session(timeout=30):
    t = telnetlib.Telnet(HOST, 23, timeout)
    t.read_until(b"login:", timeout)
    t.write(USER.encode() + b"\n")
    idx, _, _ = t.expect([rb"TERM.*\)", rb"[#$%>] ?$", rb"Password:"], timeout)
    if idx == 2:
        t.write(PASS.encode() + b"\n")
        idx, _, _ = t.expect([rb"TERM.*\)", rb"[#$%>] ?$"], timeout)
        idx = 0 if idx == 0 else 1
    if idx == 0:
        t.write(b"vt100\n")
        time.sleep(1)
    t.write(b"exec /bin/sh\n")
    time.sleep(0.5)
    t.write(b"PS1='IRUN$ '; PS2=''; export PS1 PS2; stty -echo 2>/dev/null\n")
    t.read_until(PROMPT, timeout)
    t.read_until(PROMPT, 2)
    return t


LINE_MAX = 200   # the terminal's canonical line holds 255 bytes; past that it truncates


def as_script(t, cmd):
    """A command too long for one terminal line, sent as a file to run."""
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
        f.write(cmd + "\n")
    try:
        put(t, f.name, XFER_DIR + "/cmd.sh")
    finally:
        os.unlink(f.name)
    return "sh %s/cmd.sh" % XFER_DIR


def run(t, cmd, timeout=600, out=sys.stdout):
    """Run, stream the output to `out`, return the exit status as a string."""
    if len(cmd) > LINE_MAX:
        cmd = as_script(t, cmd)
    t.write(cmd.encode() + b"; echo " + MARK + b"$?\n")
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = t.read_very_eager()
        except EOFError:
            break
        if not chunk:
            time.sleep(0.05)
            continue
        buf += chunk
        i = buf.find(MARK)
        if i >= 0:
            if out:
                out.write(buf[:i].decode("latin1").replace("\r", ""))
                out.flush()
            rest = buf[i + len(MARK):]
            while b"\n" not in rest:
                rest += t.read_some()
            if PROMPT not in rest:   # the prompt often came in the same read
                t.read_until(PROMPT, 5)
            return rest.split(b"\n")[0].decode().strip()
        j = buf.rfind(b"\n")
        if j >= 0:
            if out:
                out.write(buf[:j + 1].decode("latin1").replace("\r", ""))
                out.flush()
            buf = buf[j + 1:]
    return "TIMEOUT"


def capture(t, cmd, timeout=600):
    class S:
        def __init__(self): self.v = []
        def write(self, s): self.v.append(s)
        def flush(self): pass
    s = S()
    rc = run(t, cmd, timeout, s)
    return rc, "".join(s.v)


def sysv_sum(data):
    s = sum(data) & 0xffffffff
    r = (s & 0xffff) + (s >> 16)
    return (r & 0xffff) + (r >> 16), (len(data) + 511) // 512


import socket, threading

XFER_DIR = "/usr/tmp/cdv-xfer"
XFER_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "irix", "xfer.c")
_xfer_ready = {}


def my_ip(t):
    return t.sock.getsockname()[0]


def ensure_xfer(t):
    """Upload and build xfer once (re-done when its source changes)."""
    if _xfer_ready.get(id(t)):
        return
    src = open(XFER_SRC, "rb").read()
    want = "%d %d" % sysv_sum(src)
    rc, out = capture(t, "sum %s/xfer.c 2>/dev/null; ls %s/xfer 2>/dev/null" % (XFER_DIR, XFER_DIR))
    out = out.replace("IRUN$ ", "")
    if not (out.strip().startswith(want) and (XFER_DIR + "/xfer") in out):
        run(t, "mkdir -p %s" % XFER_DIR, out=None)
        typed_put(t, XFER_SRC, XFER_DIR + "/xfer.c")
        rc = run(t, "cd %s && cc -O -o xfer xfer.c" % XFER_DIR)
        if rc != "0":
            raise SystemExit("could not build xfer on the IRIX side")
    _xfer_ready[id(t)] = True


def _serve(data=None, sink=None):
    """A one-shot listener: send `data`, or collect into `sink` (a list)."""
    ls = socket.socket()
    ls.bind(("", 0))
    ls.listen(1)
    ls.settimeout(120)

    def worker():
        try:
            c, _ = ls.accept()
        except OSError:
            return
        with c:
            if data is not None:
                c.sendall(data)
            else:
                while True:
                    b = c.recv(65536)
                    if not b:
                        break
                    sink.append(b)
        ls.close()
    th = threading.Thread(target=worker, daemon=True)
    th.start()
    return ls.getsockname()[1], th


def put(t, local, remote):
    ensure_xfer(t)
    data = open(local, "rb").read()
    port, th = _serve(data=data)
    rc, out = capture(t, "%s/xfer get %s %d %s && chmod %s %s && sum %s" % (
        XFER_DIR, my_ip(t), port, remote, "755" if os.access(local, os.X_OK) else "644",
        remote, remote))
    th.join(5)
    want = "%d %d" % sysv_sum(data)
    got = out.replace("IRUN$ ", "").strip().split("\n")[-1].strip()
    if not got.startswith(want):
        raise SystemExit("put %s: %r, expected sum %s" % (local, out.strip(), want))
    return len(data)


def typed_put(t, local, remote):
    data = open(local, "rb").read()
    mode = "755" if os.access(local, os.X_OK) else "644"
    lines = [b"begin %s %s" % (mode.encode(), remote.encode())]
    for i in range(0, len(data), 45):
        lines.append(binascii.b2a_uu(data[i:i + 45]).rstrip(b"\n"))
    lines += [b"`", b"end"]
    tmp = remote + ".uu"
    t.write(b"cat > %s <<'UUEOF'\n" % tmp.encode())
    for k in range(0, len(lines), 8):
        t.write(b"\n".join(lines[k:k + 8]) + b"\n")
        time.sleep(0.15)
        # Keep the connection drained: if the terminal echoes, unread echo
        # fills the window and the far side stops reading what we send.
        try:
            t.read_very_eager()
        except EOFError:
            raise SystemExit("connection lost")
    t.write(b"UUEOF\n")
    rc, out = capture(t, "rm -f %s; uudecode %s; rm -f %s; sum %s" % (remote, tmp, tmp, remote))
    want = "%d %d" % sysv_sum(data)
    got = out.replace("IRUN$ ", "").strip().split("\n")[-1].strip()
    if not got.startswith(want):
        raise SystemExit("put %s: checksum %r, expected %s" % (local, got, want))
    return len(data)


def uu_line(ln):
    n = (ord(ln[0]) - 32) & 63
    out = bytearray()
    ch = [((ord(c) - 32) & 63) for c in ln[1:]] + [0] * 4
    for i in range(0, (n + 2) // 3 * 4, 4):
        a, b, c, d = ch[i:i + 4]
        out += bytes([(a << 2 | b >> 4) & 255, (b << 4 | c >> 2) & 255, (c << 6 | d) & 255])
    return bytes(out[:n])


def get(t, remote, local):
    ensure_xfer(t)
    sink = []
    port, th = _serve(sink=sink)
    rc, out = capture(t, "%s/xfer put %s %d %s; sum %s" % (XFER_DIR, my_ip(t), port, remote, remote))
    th.join(30)
    data = b"".join(sink)
    want = "%d %d" % sysv_sum(data)
    if want not in out:
        raise SystemExit("get %s: %r, expected sum %s" % (remote, out.strip(), want))
    open(local, "wb").write(data)
    return data


def typed_get(t, remote, local):
    rc, txt = capture(t, "uuencode %s x" % remote)
    data, started = b"", False
    for ln in txt.split("\n"):
        if ln.startswith("begin "):
            started = True
            continue
        if not started or not ln:
            continue
        if ln.strip() == "end":
            break
        data += uu_line(ln)
    rc, out = capture(t, "sum %s" % remote)
    want = "%d %d" % sysv_sum(data)
    if not out.replace("IRUN$ ", "").strip().startswith(want):
        raise SystemExit("get %s: checksum mismatch (%r vs %s)" % (remote, out.strip(), want))
    open(local, "wb").write(data)
    return data


def link(t, remote_dir, objdir="build/obj"):
    """Upload the objects that changed since the last link, then link there."""
    state_path = os.path.join(objdir, ".indigo-sent.json")
    try:
        sent = json.load(open(state_path))
    except (OSError, ValueError):
        sent = {}
    if sent.get("_dir") != remote_dir or sent.get("_host") != HOST:
        sent = {"_dir": remote_dir, "_host": HOST}
    run(t, "test -d %s || mkdir -p %s" % (remote_dir, remote_dir))
    names = sorted(f for f in os.listdir(objdir) if f.endswith(".o"))
    total = 0
    for f in names:
        h = hashlib.sha1(open(os.path.join(objdir, f), "rb").read()).hexdigest()
        if sent.get(f) == h:
            continue
        total += put(t, os.path.join(objdir, f), "%s/%s" % (remote_dir, f))
        sent[f] = h
        json.dump(sent, open(state_path, "w"))
    print("uploaded %d bytes" % total)
    # The command is longer than a terminal line may be (255 bytes in
    # canonical mode), so it travels as a script.
    put(t, os.path.join(objdir, "LINK"), "%s/LINK.sh" % remote_dir)
    rc = run(t, "cd %s && sh LINK.sh 2>&1 | grep -v 'compact relocation'" % remote_dir)
    if objdir != "build/obj":
        return
    rc, out = capture(t, "cd %s && ls -l C-Desk-Vint" % remote_dir)
    print(out.strip())
    if "C-Desk-Vint" not in out or "No such" in out:
        raise SystemExit("link failed")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    t = session()
    cmd = sys.argv[1]
    if cmd == "run":
        rc = run(t, " ".join(sys.argv[2:]), int(os.environ.get("IRUN_TIMEOUT", "600")))
        sys.exit(0 if rc == "0" else 1)
    elif cmd == "spawn":
        # A program left running in the background keeps the session's
        # terminal, and the shell's end-of-command marker never comes; so
        # do not wait for it.
        c = " ".join(sys.argv[3:])
        if len(c) > LINE_MAX:
            c = as_script(t, c)
        t.write(c.encode() + b"\n")
        end = time.time() + float(sys.argv[2])
        while time.time() < end:
            try:
                b = t.read_very_eager()
            except EOFError:
                break
            if b:
                sys.stdout.write(b.decode("latin1").replace("\r", ""))
                sys.stdout.flush()
            time.sleep(0.1)
        print()
    elif cmd == "put":
        a = sys.argv[2:]
        for i in range(0, len(a), 2):
            print("put %s -> %s: %d bytes" % (a[i], a[i + 1], put(t, a[i], a[i + 1])))
    elif cmd == "get":
        get(t, sys.argv[2], sys.argv[3])
    elif cmd == "linkobj":   # linkobj OBJDIR REMOTEDIR: any object set with a LINK file
        link(t, sys.argv[3], sys.argv[2])
    elif cmd == "link":
        d = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("INDIGO_DIR", "/usr/tmp/cdv-build")
        link(t, d)
        get(t, d + "/C-Desk-Vint", "build/C-Desk-Vint")
        os.chmod("build/C-Desk-Vint", 0o755)
        print("fetched build/C-Desk-Vint")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
