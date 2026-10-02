#!/usr/bin/env python3
"""Make a GNU o32 object acceptable to IRIX 5.3's ld.

GCC (with GNU as) produces ordinary SVR4 MIPS o32 code, which is what IRIX 5
runs, but IRIX's ld is stricter about the container than GNU's tools are:

1. Sections GNU adds and IRIX does not know (.MIPS.abiflags, .gnu.attributes,
   .note.GNU-stack, .pdr, .comment, .mdebug.abi32) are dropped.
2. EF_MIPS_ABI_O32 is a GNU addition to e_flags; it is cleared.
3. Every object must carry .mdebug, the ECOFF symbolic header (assemble with
   -Wa,-mdebug). Its header holds ABSOLUTE file offsets, which objcopy does not
   adjust when it moves the section, so they are rebased here.
4. IRIX's ld pairs the i-th global of .symtab with the i-th external of
   .mdebug, and expects the locals of .symtab to be only the null symbol and
   section symbols (static functions live in .mdebug alone, as IDO emits them).
   Gas also lists some statics among the externals, and keeps static functions
   and section symbols for .reginfo/.mdebug in .symtab. Both tables are
   rewritten into the IRIX shape, and every relocation renumbered to match.

    irixobj.py file.o...     (in place)
"""
import os, struct, subprocess, sys

OBJCOPY = os.environ.get("IRIX_OBJCOPY", "mips-linux-gnu-objcopy")
HDRR = ">hhiiiiiiiiiiiiiiiiiiiiiii"
OFFSET_FIELDS = [4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24]   # the cb*Offset fields
SHT_SYMTAB, SHT_REL, STT_SECTION = 2, 9, 3


def sections(b):
    """name -> (name, type, flags, addr, offset, size, link, info, align, entsize, index)"""
    shoff, = struct.unpack(">I", b[32:36])
    shentsize, shnum, shstrndx = struct.unpack(">HHH", b[46:52])
    sh = [struct.unpack(">IIIIIIIIII", b[shoff + i * shentsize:shoff + (i + 1) * shentsize])
          for i in range(shnum)]
    stro = sh[shstrndx][4]
    name = lambda o: b[stro + o:b.index(b"\0", stro + o)].decode()
    return {name(s[0]): s + (i,) for i, s in enumerate(sh)}


def section_header_offset(b, index):
    shoff, = struct.unpack(">I", b[32:36])
    shentsize, = struct.unpack(">H", b[46:48])
    return shoff + index * shentsize


def cstr(b, o):
    return bytes(b[o:b.index(b"\0", o)])


def rewrite_symtab(b):
    """Keep null, section symbols for allocated sections, and globals; renumber relocs."""
    S = sections(b)
    by_index = {s[10]: (n, s) for n, s in S.items()}
    st = S[".symtab"]
    count, first_global = st[5] // 16, st[7]
    syms = [bytes(b[st[4] + 16 * i:st[4] + 16 * i + 16]) for i in range(count)]
    referenced = set()
    rels = [s for s in S.values() if s[1] == SHT_REL]
    for r in rels:
        for k in range(r[5] // 8):
            info, = struct.unpack(">I", b[r[4] + 8 * k + 4:r[4] + 8 * k + 8])
            referenced.add(info >> 8)
    keep = []
    for i, e in enumerate(syms):
        if i >= first_global or i == 0:
            keep.append(i)
            continue
        info, shndx = e[12], struct.unpack(">H", e[14:16])[0]
        if info & 15 == STT_SECTION:
            sec_name = by_index.get(shndx, ("",))[0]
            if i in referenced or sec_name in (".text", ".data", ".bss", ".rodata", ".sdata", ".sbss"):
                keep.append(i)
            continue
        if i in referenced:
            sys.exit("relocation against local symbol %d (%s); not supported"
                     % (i, cstr(b, list(S.values())[st[6]][4] + struct.unpack(">I", e[0:4])[0])))
    remap = {old: new for new, old in enumerate(keep)}
    new_first_global = sum(1 for i in keep if i < first_global)
    b[st[4]:st[4] + 16 * count] = b"".join(syms[i] for i in keep) + b"\0" * 16 * (count - len(keep))
    sho = section_header_offset(b, st[10])
    b[sho + 20:sho + 24] = struct.pack(">I", 16 * len(keep))          # sh_size
    b[sho + 28:sho + 32] = struct.pack(">I", new_first_global)        # sh_info
    for r in rels:
        for k in range(r[5] // 8):
            o = r[4] + 8 * k + 4
            info, = struct.unpack(">I", b[o:o + 4])
            b[o:o + 4] = struct.pack(">I", remap[info >> 8] << 8 | (info & 255))


def elf_globals(b):
    S = sections(b)
    st = S[".symtab"]
    strt = list(S.values())[st[6]]
    out = []
    for i in range(st[7], st[5] // 16):
        nm_o, = struct.unpack(">I", b[st[4] + 16 * i:st[4] + 16 * i + 4])
        out.append(cstr(b, strt[4] + nm_o))
    return out


def match_externals(b, md):
    """Rebuild .mdebug's external table in .symtab's global order."""
    hdr = list(struct.unpack(HDRR, b[md:md + 0x60]))
    iext, extoff, ssext = hdr[23], hdr[24], hdr[18]
    ents = {}
    for i in range(iext):
        e = bytes(b[extoff + 16 * i:extoff + 16 * i + 16])
        iss, = struct.unpack(">I", e[4:8])
        ents.setdefault(cstr(b, ssext + iss), e)
    new = []
    for name in elf_globals(b):
        if name not in ents:
            sys.exit("global %s has no .mdebug external" % name.decode())
        new.append(ents[name])
    if len(new) > iext:
        sys.exit("more ELF globals than .mdebug externals")
    b[extoff:extoff + 16 * len(new)] = b"".join(new)
    hdr[23] = len(new)
    b[md:md + 0x60] = struct.pack(HDRR, *hdr)


def fix(f):
    old = sections(open(f, "rb").read()).get(".mdebug")
    if old is None:
        sys.exit(f"{f}: no .mdebug (assemble with -Wa,-mdebug)")
    tmp = f + ".irixobj"
    subprocess.check_call([OBJCOPY,
        "-R", ".MIPS.abiflags", "-R", ".gnu.attributes", "-R", ".note.GNU-stack",
        "-R", ".pdr", "-R", ".rel.pdr", "-R", ".comment", "-R", ".mdebug.abi32", f, tmp])
    b = bytearray(open(tmp, "rb").read())
    new = sections(b)[".mdebug"]
    delta = new[4] - old[4]
    hdr = list(struct.unpack(HDRR, b[new[4]:new[4] + 0x60]))
    for i in OFFSET_FIELDS:
        if hdr[i]:
            hdr[i] += delta
    b[new[4]:new[4] + 0x60] = struct.pack(HDRR, *hdr)
    rewrite_symtab(b)
    match_externals(b, new[4])
    flags, = struct.unpack(">I", b[36:40])
    b[36:40] = struct.pack(">I", flags & ~0x0000F000)
    open(tmp, "wb").write(b)
    os.replace(tmp, f)


if __name__ == "__main__":
    for f in sys.argv[1:]:
        fix(f)
