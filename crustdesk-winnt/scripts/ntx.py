#!/usr/bin/env python3
"""Talk to tools/nt/ntexec.c running on a Windows NT machine.

    ntx.py run 'command'           run under cmd /c, print output, exit with its code
    ntx.py spawn 'command'         start a detached process
    ntx.py put LOCAL... REMOTEDIR  copy files to the machine (REMOTEDIR ends in \\)
    ntx.py put LOCAL REMOTE        copy one file
    ntx.py get REMOTE LOCAL        copy a file back
    ntx.py sync DIR REMOTEDIR      copy every file in DIR whose content changed

Environment: NTX (host:port, default 127.0.0.1:2370 -- the QEMU forward).
"""
import hashlib, json, os, socket, sys

HOST, PORT = os.environ.get("NTX", "127.0.0.1:2370").rsplit(":", 1)
PORT = int(PORT)


def conn(line):
    s = socket.create_connection((HOST, PORT), 30)
    s.settimeout(None)
    s.sendall(line.encode("latin-1") + b"\n")
    return s


def readall(s):
    out = []
    while True:
        b = s.recv(65536)
        if not b:
            return b"".join(out)
        out.append(b)


def run(cmd, echo=True):
    s = conn("RUN " + cmd)
    buf = b""
    while True:
        b = s.recv(4096)
        if not b:
            break
        buf += b
        if echo:
            # hold back the tail where the exit marker may be arriving
            cut = buf.rfind(b"\n@@EXIT ")
            if cut < 0:
                cut = max(0, len(buf) - 16)
            sys.stdout.buffer.write(buf[:cut].replace(b"\r\n", b"\n"))
            sys.stdout.flush()
            buf = buf[cut:]
    i = buf.rfind(b"\n@@EXIT ")
    code = int(buf[i + 8:].strip()) if i >= 0 else 255
    if echo:
        sys.stdout.buffer.write(buf[:max(i, 0)].replace(b"\r\n", b"\n") + b"\n")
    return code


def put(local, remote):
    data = open(local, "rb").read()
    s = conn("PUT %s %d" % (remote, len(data)))
    s.sendall(data)
    r = readall(s).decode().strip()
    if r != "OK":
        raise SystemExit("put %s -> %s: %s" % (local, remote, r))


def get(remote, local):
    s = conn("GET " + remote)
    data = readall(s)
    nl = data.index(b"\n")
    size = int(data[:nl])
    if size < 0:
        raise SystemExit("get %s: no such file" % remote)
    body = data[nl + 1:]
    if len(body) != size:
        raise SystemExit("get %s: short read %d/%d" % (remote, len(body), size))
    open(local, "wb").write(body)


def sync(local_dir, remote_dir, stamp=None):
    """Upload files under local_dir that changed since the last sync."""
    stamp = stamp or os.path.join(local_dir, ".ntx-sync")
    try:
        seen = json.load(open(stamp))
    except (OSError, ValueError):
        seen = {}
    if seen.get("@remote") != remote_dir:
        seen = {"@remote": remote_dir}
    run('if not exist "%s" mkdir "%s"' % (remote_dir, remote_dir), echo=False)
    made = set()
    n = 0
    for root, dirs, files in os.walk(local_dir):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for f in sorted(files):
            if f.startswith("."):
                continue
            p = os.path.join(root, f)
            rel = os.path.relpath(p, local_dir)
            h = hashlib.sha1(open(p, "rb").read()).hexdigest()
            if seen.get(rel) == h:
                continue
            rdir = os.path.dirname(rel)
            if rdir and rdir not in made:
                run('if not exist "%s\\%s" mkdir "%s\\%s"' % (
                    remote_dir, rdir.replace("/", "\\"), remote_dir, rdir.replace("/", "\\")), echo=False)
                made.add(rdir)
            put(p, remote_dir + "\\" + rel.replace("/", "\\"))
            seen[rel] = h
            n += 1
    json.dump(seen, open(stamp, "w"))
    return n


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    if a[0] == "run":
        sys.exit(run(" ".join(a[1:])))
    elif a[0] == "spawn":
        print(readall(conn("SPAWN " + " ".join(a[1:]))).decode().strip())
    elif a[0] == "put":
        dst = a[-1]
        for src in a[1:-1]:
            put(src, dst + os.path.basename(src) if dst.endswith("\\") else dst)
    elif a[0] == "get":
        get(a[1], a[2])
    elif a[0] == "sync":
        print("%d file(s) sent" % sync(a[1], a[2]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        sys.stderr.close()
