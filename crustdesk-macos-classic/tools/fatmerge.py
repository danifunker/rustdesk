#!/usr/bin/env python3
"""Join a 68k and a PowerPC build of the same application into one fat one.

    tools/fatmerge.py 68K.bin PPC.bin OUT.bin

Both inputs and the output are MacBinary II. A fat application is the 68k
application's resource fork (CODE and the rest), plus the PowerPC build's
'cfrg' resource, which points the Code Fragment Manager at the PEF container
in the data fork -- taken from the PowerPC build too. A PowerPC Mac sees the
cfrg and runs the PEF; a 68k Mac knows nothing of cfrg and runs CODE.

Every other resource comes from the 68k build; where both have one of the
same type and ID, the 68k one wins, except cfrg.
"""
import struct
import sys


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def read_macbinary(path):
    d = open(path, 'rb').read()
    hdr = d[:128]
    dlen, rlen = struct.unpack('>II', hdr[83:91])
    data = d[128:128 + dlen]
    roff = 128 + ((dlen + 127) // 128) * 128
    rsrc = d[roff:roff + rlen]
    return hdr, data, rsrc


def write_macbinary(path, hdr, data, rsrc):
    h = bytearray(hdr)
    h[83:91] = struct.pack('>II', len(data), len(rsrc))
    h[124:126] = struct.pack('>H', crc16_xmodem(bytes(h[:124])))
    pad = lambda b: b + bytes((-len(b)) % 128)
    open(path, 'wb').write(bytes(h) + pad(data) + pad(rsrc))


def read_fork(f):
    """[(type, id, name or None, attrs, data)] from a raw resource fork."""
    if not f:
        return []
    doff, moff, dlen, mlen = struct.unpack('>IIII', f[:16])
    m = f[moff:moff + mlen]
    tl, nl = struct.unpack('>HH', m[24:28])
    ntypes = struct.unpack('>h', m[tl:tl + 2])[0] + 1
    out = []
    for t in range(ntypes):
        e = tl + 2 + 8 * t
        rtype = m[e:e + 4]
        count, refoff = struct.unpack('>hH', m[e + 4:e + 8])
        for r in range(count + 1):
            ref = tl + refoff + 12 * r
            rid, noff = struct.unpack('>hh', m[ref:ref + 4])
            attrs = m[ref + 4]
            dof = int.from_bytes(m[ref + 5:ref + 8], 'big')
            name = None
            if noff != -1:
                n = m[nl + noff]
                name = m[nl + noff + 1:nl + noff + 1 + n]
            ln = struct.unpack('>I', f[doff + dof:doff + dof + 4])[0]
            out.append((rtype, rid, name, attrs, f[doff + dof + 4:doff + dof + 4 + ln]))
    return out


def write_fork(resources):
    types = {}
    for r in resources:
        types.setdefault(r[0], []).append(r)
    data = bytearray()
    names = bytearray()
    typelist = bytearray(struct.pack('>h', len(types) - 1))
    reflists = bytearray()
    reflist_base = 2 + 8 * len(types)
    for rtype, rs in types.items():
        typelist += rtype + struct.pack('>hH', len(rs) - 1, reflist_base + len(reflists))
        for rtype_, rid, name, attrs, body in rs:
            noff = -1
            if name is not None:
                noff = len(names)
                names += bytes([len(name)]) + name
            reflists += struct.pack('>hh', rid, noff) + bytes([attrs]) + \
                len(data).to_bytes(3, 'big') + bytes(4)
            data += struct.pack('>I', len(body)) + body
    tl = 28
    nl = tl + len(typelist) + len(reflists)
    mapbody = bytearray(16) + bytes(4) + bytes(2) + bytes(2) + struct.pack('>HH', tl, nl)
    mapbody += typelist + reflists + names
    doff = 256
    moff = doff + len(data)
    header = struct.pack('>IIII', doff, moff, len(data), len(mapbody))
    mapbody[0:16] = header
    return header + bytes(doff - 16) + bytes(data) + bytes(mapbody)


def main():
    h68, _, r68 = read_macbinary(sys.argv[1])
    _, pef, rppc = read_macbinary(sys.argv[2])
    res68, resppc = read_fork(r68), read_fork(rppc)
    cfrg = [r for r in resppc if r[0] == b'cfrg']
    if not cfrg or not pef.startswith(b'Joy!peff'):
        sys.exit('the PowerPC build has no cfrg or no PEF data fork')
    merged = [r for r in res68 if r[0] != b'cfrg'] + cfrg
    write_macbinary(sys.argv[3], h68, pef, write_fork(merged))
    kinds = sorted({r[0].decode('mac_roman') for r in merged})
    print('fat: %d-byte PEF, %d resources (%s)' % (len(pef), len(merged), ' '.join(kinds)))


if __name__ == '__main__':
    main()
