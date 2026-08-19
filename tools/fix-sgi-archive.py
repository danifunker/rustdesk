#!/usr/bin/env python3
"""Repair sh_info on the symbol tables in an SGI static archive.

SGI's own .a files carry `sh_info = 0` on their `.symtab` sections. The ELF
spec says that field is the index of the first non-local symbol, and zero
cannot be right because symbol 0 is always the reserved local one. GNU ld and
IRIX's own linker ignore the field; LLD checks it and refuses the object:

    ld.lld-irix: error: libXtst.a(XTest.o): invalid sh_info in symbol table

That is why linking XTEST fails while every shared library works -- the shared
objects were produced by a different path and have the field right.

This rewrites sh_info to the real index of the first global symbol, which is
what every consumer would have computed anyway, and leaves everything else
untouched. Verified by relinking: the repaired archive links and the resulting
binary runs.

    fix-sgi-archive.py <in.a> <out.a>
"""
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SHT_SYMTAB = 2
STB_LOCAL = 0


def fix_object(path: Path) -> int:
    """Return the number of symbol tables repaired in one ELF object."""
    data = bytearray(path.read_bytes())
    if data[:4] != b"\x7fELF":
        return 0
    if data[4] != 1:
        raise SystemExit(f"{path}: only ELF32 is handled, this is class {data[4]}")
    big = data[5] == 2
    end = ">" if big else "<"

    (e_shoff,) = struct.unpack_from(end + "I", data, 32)
    (e_shentsize,) = struct.unpack_from(end + "H", data, 46)
    (e_shnum,) = struct.unpack_from(end + "H", data, 48)
    if e_shoff == 0 or e_shnum == 0:
        return 0

    fixed = 0
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from(end + "I", data, base + 4)
        if sh_type != SHT_SYMTAB:
            continue
        sh_offset, sh_size, sh_link, sh_info = struct.unpack_from(
            end + "IIII", data, base + 16
        )
        (sh_entsize,) = struct.unpack_from(end + "I", data, base + 36)
        if sh_entsize == 0:
            sh_entsize = 16
        count = sh_size // sh_entsize

        first_global = count  # all local is legal; sh_info is then the count
        for n in range(count):
            st_info = data[sh_offset + n * sh_entsize + 12]
            if (st_info >> 4) != STB_LOCAL:
                first_global = n
                break

        if sh_info != first_global:
            struct.pack_into(end + "I", data, base + 28, first_global)
            fixed += 1

    if fixed:
        path.write_bytes(bytes(data))
    return fixed


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        subprocess.run(["ar", "x", str(src.resolve())], cwd=work, check=True)
        members = sorted(p for p in work.iterdir() if p.is_file())
        total = 0
        for m in members:
            n = fix_object(m)
            total += n
            print(f"  {m.name}: {'repaired ' + str(n) if n else 'already correct'}")
        out = work / "fixed.a"
        subprocess.run(["ar", "rcs", str(out)] + [m.name for m in members],
                       cwd=work, check=True)
        shutil.copy(out, dst)
    print(f"{src} -> {dst}: {total} symbol table(s) repaired")


if __name__ == "__main__":
    main()
