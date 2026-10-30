#!/usr/bin/env python3
"""Will Windows load this program? Every function it imports, looked up in
the exports of a set of system DLLs (Windows 95's, 98's, NT's...).

    check-imports.py EXE DLLDIR

Windows 9x refuses to start a program whose import table names a function
its DLLs do not export -- not when the call is made, but at load, with "a
device attached to the system is not functioning". So the x86 build, which
is meant to run on 95 and 98 as well as NT, must import nothing NT-only:
those calls are looked up at run time (src/ntapi.c). This says which
imports are missing; DLLs not in DLLDIR are reported, not judged.

Exits 1 if anything is missing. Plain Python, reading the PE files directly.
"""
import os
import struct
import sys


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec, optsize = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    secs = []
    for i in range(nsec):
        o = opt + optsize + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, max(vsize, rsize), raw))
    return opt, secs


def rva2off(secs, rva):
    for va, size, raw in secs:
        if va <= rva < va + size:
            return rva - va + raw
    raise ValueError("rva %x outside the sections" % rva)


def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("latin-1")


def directory(d, opt, n):
    return struct.unpack_from("<II", d, opt + 96 + 8 * n)  # PE32: data directories at +96


def exports(path):
    d = open(path, "rb").read()
    opt, secs = sections(d)
    rva, size = directory(d, opt, 0)
    if not rva:
        return set(), set()
    e = rva2off(secs, rva)
    base, nfun, nname, afun, aname, aord = struct.unpack_from("<IIIIII", d, e + 16)
    names = set()
    for i in range(nname):
        p = struct.unpack_from("<I", d, rva2off(secs, aname) + 4 * i)[0]
        names.add(cstr(d, rva2off(secs, p)))
    return names, set(range(base, base + nfun))


def imports(path):
    d = open(path, "rb").read()
    opt, secs = sections(d)
    rva, size = directory(d, opt, 1)
    out = {}
    o = rva2off(secs, rva)
    while True:
        ilt, _, _, name, iat = struct.unpack_from("<IIIII", d, o)
        if not name:
            break
        dll = cstr(d, rva2off(secs, name)).lower()
        t = rva2off(secs, ilt or iat)
        while True:
            v = struct.unpack_from("<I", d, t)[0]
            if not v:
                break
            if v & 0x80000000:
                out.setdefault(dll, []).append(v & 0xFFFF)
            else:
                out.setdefault(dll, []).append(cstr(d, rva2off(secs, v) + 2))
            t += 4
        o += 20
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    exe, dlldir = sys.argv[1], sys.argv[2]
    have = {n.lower(): n for n in os.listdir(dlldir)}
    missing = 0
    for dll, funcs in sorted(imports(exe).items()):
        if dll not in have:
            print("%-14s (not in %s: not checked)" % (dll, dlldir))
            continue
        names, ords = exports(os.path.join(dlldir, have[dll]))
        gone = [f for f in funcs if (f not in ords if isinstance(f, int) else f not in names)]
        print("%-14s %3d imported, %d missing%s" % (dll, len(funcs), len(gone),
                                                   (": " + " ".join(map(str, gone))) if gone else ""))
        missing += len(gone)
    sys.exit(1 if missing else 0)


if __name__ == "__main__":
    main()
