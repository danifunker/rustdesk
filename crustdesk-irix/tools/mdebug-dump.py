#!/usr/bin/env python3
"""Compare an object's ELF global symbols with its .mdebug external symbols."""
import sys, struct
sys.path.insert(0, __import__("os").path.dirname(__file__))
from irixobj import sections
b = open(sys.argv[1], "rb").read()
S = sections(b)
md = S[".mdebug"][4]
h = struct.unpack(">hhiiiiiiiiiiiiiiiiiiiiiii", b[md:md+0x60])
iextMax, cbExtOffset, cbSsExtOffset = h[23], h[24], h[18]
ext = []
for i in range(iextMax):
    o = cbExtOffset + 16*i
    flags, ifd, iss, value, bits = struct.unpack(">HhIII", b[o:o+16])
    nm = b[cbSsExtOffset+iss:b.index(b"\0", cbSsExtOffset+iss)].decode()
    ext.append((nm, ifd, bits >> 26, (bits >> 21) & 31))
st = S[".symtab"]; strt = list(S.values())[st[6]]
syms = []
for i in range(st[5] // 16):
    nm_o, val, size, info, other, shndx = struct.unpack(">IIIBBH", b[st[4]+16*i:st[4]+16*i+16])
    nm = b[strt[4]+nm_o:b.index(b"\0", strt[4]+nm_o)].decode()
    syms.append((i, nm, info >> 4, info & 15, shndx))
first_global = st[7]
print("symtab: %d symbols, first global %d" % (len(syms), first_global))
for s in syms: print("  sym", s)
print("mdebug ext: %d" % len(ext))
for i, e in enumerate(ext): print("  ext", i, e)
