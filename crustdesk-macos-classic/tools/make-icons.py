#!/usr/bin/env python3
"""C-Desk-Vint's Finder icons: a compact Mac showing RustDesk's badge.

    tools/make-icons.py rsrc/icons.r [preview.png]

Draws the icon pixel by pixel at 32x32 and 16x16 (a downscaled picture
dithers to mud in 16 colours) and writes the icon family as Rez data:
'ICN#'/'ics#' (1-bit and mask), 'icl4'/'ics4' (the Mac's 16 colours) and
'icl8'/'ics8' (its 256), all ID 128, which the application's BNDL names;
and 'ICON' 128, the 1-bit picture alone, which a dialog's Icon item draws.
The badge's blue and ring follow RustDesk's own logo (128x128.png).
"""
import sys
from PIL import Image, ImageDraw

WHITE, BLACK = (255, 255, 255), (0, 0, 0)
PLATINUM, SHADOW, DARK = (221, 221, 221), (153, 153, 153), (102, 102, 102)
BLUE = (0, 102, 255)  # RustDesk's #0071FF, on the Mac's colour cube


def mac_palette8():
    """The standard 8-bit CLUT: the 6x6x6 cube from white, then ramps of red,
    green, blue and grey without the cube's values, then black."""
    steps = [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    pal = [(r, g, b) for r in steps for g in steps for b in steps][:-1]
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    pal += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp]
    pal += [(v, v, v) for v in ramp] + [(0, 0, 0)]
    assert len(pal) == 256
    return pal


PAL4 = [(255, 255, 255), (252, 243, 5), (255, 100, 2), (221, 8, 6), (242, 8, 132),
        (70, 0, 165), (0, 0, 212), (2, 171, 234), (31, 183, 20), (0, 100, 17),
        (86, 44, 5), (144, 113, 58), (192, 192, 192), (128, 128, 128), (64, 64, 64),
        (0, 0, 0)]


def nearest(pal, c):
    if pal is PAL4 and c == BLUE:
        return 6  # the 16 colours' blue: nearer by numbers is cyan, which is wrong
    return min(range(len(pal)), key=lambda i: sum((a - b) ** 2 for a, b in zip(pal[i], c)))


def draw32():
    im = Image.new('RGBA', (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # the case, with a shadow down the right and bottom
    d.rounded_rectangle((5, 1, 26, 27), 2, fill=PLATINUM, outline=BLACK)
    d.line((25, 3, 25, 25), fill=SHADOW)
    d.line((7, 26, 25, 26), fill=SHADOW)
    # the foot
    d.rectangle((7, 28, 24, 30), fill=SHADOW, outline=BLACK)
    # the screen: bezel, then RustDesk's badge
    d.rectangle((8, 4, 23, 17), fill=DARK, outline=BLACK)
    d.rectangle((9, 5, 22, 16), fill=BLUE)
    # the ring: two white arcs with gaps at top and bottom, as in the logo
    d.ellipse((11, 6, 20, 15), outline=WHITE, width=2)
    for x in (15, 16):
        for y in (6, 7, 14, 15):
            im.putpixel((x, y), BLUE + (255,))
    # the floppy slot and the Apple-ish badge corner
    d.line((15, 21, 23, 21), fill=BLACK)
    d.line((15, 22, 23, 22), fill=SHADOW)
    d.rectangle((8, 21, 10, 22), fill=DARK)
    return im


def draw16():
    im = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((2, 0, 13, 13), fill=PLATINUM, outline=BLACK)
    d.line((12, 1, 12, 12), fill=SHADOW)
    d.rectangle((3, 14, 12, 15), fill=SHADOW, outline=BLACK)
    d.rectangle((4, 2, 11, 8), fill=BLUE, outline=BLACK)
    # a four-pixel ring with a gap top and bottom
    for x, y in ((6, 4), (6, 5), (6, 6), (9, 4), (9, 5), (9, 6), (7, 3), (8, 7)):
        im.putpixel((x, y), WHITE + (255,))
    d.line((8, 10, 11, 10), fill=BLACK)
    return im


def mask_of(im):
    w, h = im.size
    return [[1 if im.getpixel((x, y))[3] else 0 for x in range(w)] for y in range(h)]


def bw_of(im):
    """1-bit: outline and dark parts black, light parts white; the screen
    black with the ring white, as a black-and-white Mac shows it."""
    w, h = im.size
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = im.getpixel((x, y))
            if not a:
                row.append(0)
            elif (r, g, b) == WHITE:
                row.append(0)
            elif (r, g, b) in (PLATINUM,):
                row.append(0)
            elif (r, g, b) == SHADOW:
                row.append(0)  # dithered, it only looks noisy at this size
            else:
                row.append(1)
        out.append(row)
    return out


def bits(rows):
    out = bytearray()
    for row in rows:
        for i in range(0, len(row), 8):
            v = 0
            for b in row[i:i + 8]:
                v = v << 1 | b
            out.append(v)
    return bytes(out)


def indexed(im, pal, nbits):
    w, h = im.size
    vals = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = im.getpixel((x, y))
            vals.append(nearest(pal, (r, g, b)) if a else 0)
    if nbits == 8:
        return bytes(vals)
    return bytes(vals[i] << 4 | vals[i + 1] for i in range(0, len(vals), 2))


def rez(kind, data):
    hexs = data.hex().upper()
    lines = ['    $"%s"' % hexs[i:i + 64] for i in range(0, len(hexs), 64)]
    return "data '%s' (128) {\n%s\n};\n" % (kind, '\n'.join(lines))


def main():
    big, small = draw32(), draw16()
    p8 = mac_palette8()
    out = ['/* Generated by tools/make-icons.py. Do not edit. */', '']
    out.append(rez('ICN#', bits(bw_of(big)) + bits(mask_of(big))))
    out.append(rez('ICON', bits(bw_of(big))))  # for dialogs' Icon items
    out.append(rez('icl4', indexed(big, PAL4, 4)))
    out.append(rez('icl8', indexed(big, p8, 8)))
    out.append(rez('ics#', bits(bw_of(small)) + bits(mask_of(small))))
    out.append(rez('ics4', indexed(small, PAL4, 4)))
    out.append(rez('ics8', indexed(small, p8, 8)))
    open(sys.argv[1], 'w').write('\n'.join(out))
    if len(sys.argv) > 2:
        # What the Finder will show, large: 8-bit, 4-bit, 1-bit, and the small one.
        prev = Image.new('RGB', (32 * 4 + 16 + 40, 40), (170, 170, 170))
        q8 = Image.new('RGBA', (32, 32))
        q4 = Image.new('RGBA', (32, 32))
        q1 = Image.new('RGBA', (32, 32))
        bw, mk = bw_of(big), mask_of(big)
        for y in range(32):
            for x in range(32):
                r, g, b, a = big.getpixel((x, y))
                if a:
                    q8.putpixel((x, y), p8[nearest(p8, (r, g, b))] + (255,))
                    q4.putpixel((x, y), PAL4[nearest(PAL4, (r, g, b))] + (255,))
                if mk[y][x]:
                    q1.putpixel((x, y), (BLACK if bw[y][x] else WHITE) + (255,))
        for i, q in enumerate((q8, q4, q1)):
            prev.paste(q, (4 + 40 * i, 4), q)
        prev.paste(small, (4 + 120, 4), small)
        prev.resize((prev.width * 6, prev.height * 6), Image.NEAREST).save(sys.argv[2])


main()
