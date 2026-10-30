#!/usr/bin/env python3
"""C-Desk-Vint's Windows icon: a monitor showing RustDesk's badge.

    tools/make-icon.py res/cdv.ico [preview.png]

Drawn pixel by pixel at 32x32 and 16x16, and written as an icon file NT 4.0
can read: each image a 4-bit DIB in the 16 standard VGA colours with a
1-bit mask (Pillow's .ico writer embeds PNGs, which NT 4.0 shows as blank).
The badge's blue is RustDesk's own, on the VGA palette.
"""
import struct
import sys
from PIL import Image, ImageDraw

VGA = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128),
       (0, 128, 128), (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0),
       (255, 255, 0), (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255)]
BLACK, GREY, DARK, WHITE, BLUE = 0, 7, 8, 15, 12
CLEAR = -1


def draw(size):
    """A grid of palette indices (CLEAR: transparent)."""
    px = [[CLEAR] * size for _ in range(size)]

    def rect(x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                px[y][x] = c

    s = size / 32.0
    sc = lambda v: int(round(v * s))
    # the case, with a bevel: light top-left, dark bottom-right
    rect(sc(2), sc(2), sc(29) - 1 + (size == 16), sc(23), GREY)
    rect(sc(2), sc(2), sc(29) - 1 + (size == 16), sc(2), WHITE)
    rect(sc(2), sc(2), sc(2), sc(23), WHITE)
    rect(sc(2), sc(23), sc(29) - 1 + (size == 16), sc(23), DARK)
    rect(sc(29) - 1 + (size == 16), sc(2), sc(29) - 1 + (size == 16), sc(23), DARK)
    # the screen
    x0, y0, x1, y1 = sc(5), sc(5), sc(26), sc(20)
    rect(x0 - 1, y0 - 1, x1 + 1, y1 + 1, BLACK)
    rect(x0, y0, x1, y1, BLUE)
    # the badge: a white ring with a gap, RustDesk's mark at this size
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    r = (y1 - y0) / 2.0 - (1.2 if size == 32 else 0.6)
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            ring = r - (2.2 if size == 32 else 1.4) <= d <= r
            if ring and not (x > cx and abs(y - cy) < r * 0.35):
                px[y][x] = WHITE
    # the stand
    rect(sc(12), sc(24), sc(19), sc(26), DARK)
    rect(sc(8), sc(27), sc(23), sc(29) - (size == 16), GREY)
    rect(sc(8), sc(29) - (size == 16), sc(23), sc(29) - (size == 16), BLACK)
    return px


def dib(px):
    """BITMAPINFOHEADER, palette, 4-bit XOR image, 1-bit AND mask; rows
    bottom-up, each padded to 32 bits."""
    n = len(px)
    xor = b""
    for row in reversed(px):
        line = bytearray()
        for x in range(0, n, 2):
            a = row[x] if row[x] >= 0 else 0
            b = row[x + 1] if row[x + 1] >= 0 else 0
            line.append(a << 4 | b)
        while len(line) % 4:
            line.append(0)
        xor += bytes(line)
    mask = b""
    for row in reversed(px):
        bits = 0
        line = bytearray()
        for x in range(n):
            bits = bits << 1 | (1 if row[x] == CLEAR else 0)
            if x % 8 == 7:
                line.append(bits)
                bits = 0
        while len(line) % 4:
            line.append(0)
        mask += bytes(line)
    head = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, 4, 0, len(xor) + len(mask), 0, 0, 16, 0)
    pal = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in VGA)
    return head + pal + xor + mask


def main():
    images = [draw(32), draw(16)]
    blobs = [dib(p) for p in images]
    out = struct.pack("<HHH", 0, 1, len(blobs))
    off = 6 + 16 * len(blobs)
    for p, b in zip(images, blobs):
        n = len(p)
        out += struct.pack("<BBBBHHII", n, n, 16, 0, 1, 4, len(b), off)
        off += len(b)
    out += b"".join(blobs)
    open(sys.argv[1], "wb").write(out)
    if len(sys.argv) > 2:
        prev = Image.new("RGB", (32 * 4 + 16 * 4 + 8, 32 * 4), (0, 128, 128))
        x = 0
        for p in images:
            n = len(p)
            im = Image.new("RGB", (n, n), (0, 128, 128))
            for y in range(n):
                for xx in range(n):
                    if p[y][xx] >= 0:
                        im.putpixel((xx, y), VGA[p[y][xx]])
            prev.paste(im.resize((n * 4, n * 4), Image.NEAREST), (x, 0))
            x += n * 4 + 8
        prev.save(sys.argv[2])


if __name__ == "__main__":
    main()
