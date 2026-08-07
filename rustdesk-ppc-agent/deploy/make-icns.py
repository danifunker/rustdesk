#!/usr/bin/env python3
"""Draw the application icon and write deploy/app.icns.

Run it when the icon changes; the .icns is committed, so a build does not need
Python or PIL:

    ./deploy/make-icns.py

WHY THE FILE IS WRITTEN BY HAND. There is no icns encoder available on either
machine: Leopard's `sips` refuses `-s format icns`, `iconutil` is 10.7+, and
Icon Composer is a GUI. So this emits the container itself.

WHAT GOES IN IT, and why each entry. Leopard reads all four:

    is32 / s8mk    16x16    RGB (RLE) + 8-bit mask
    il32 / l8mk    32x32    the sizes the Finder list and menus use
    it32 / t8mk   128x128   with a leading four zero bytes, which only this
                            type has, and which nothing explains but every
                            reader expects
    ic08          256x256   a whole PNG, understood from 10.5

The 32-bit types are RLE-compressed per colour plane, R then G then B, with the
alpha carried separately in the mask entry -- so the "32-bit" name is a lie and
feeding it RGBA produces an icon that decodes as noise. The encoder below is
checked against its own decoder before anything is written, because a malformed
icns does not error: it renders as the blank-page placeholder, which looks
exactly like having forgotten to set CFBundleIconFile.
"""
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("this needs Pillow: pip install Pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "app.icns")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


# --------------------------------------------------------------------------
# The drawing. A display on a stand, because that is what the thing does, with
# a small amber dot for the remote pointer -- the one detail that says this is
# a machine someone else is driving.
# --------------------------------------------------------------------------
def draw(size):
    S = 1024                      # drawn big, downsampled once at the end
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Rounded-square body with a vertical wash. Two flat colours banded down the
    # square read as a gradient at every size we emit and cost nothing.
    top, bottom = (38, 92, 160), (22, 48, 92)
    r = int(S * 0.22)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=r, fill=top)
    for i in range(S // 2, S):
        t = (i - S // 2) / float(S // 2)
        c = tuple(int(top[k] + (bottom[k] - top[k]) * t) for k in range(3))
        d.line([(0, i), (S, i)], fill=c + (255,))
    # Re-cut the corners: the banding above painted over them.
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=r, fill=255)
    img.putalpha(mask)
    d = ImageDraw.Draw(img)

    # The screen.
    sx0, sy0, sx1, sy1 = S * 0.20, S * 0.24, S * 0.80, S * 0.62
    d.rounded_rectangle([sx0, sy0, sx1, sy1], radius=int(S * 0.035),
                        fill=(238, 244, 250, 255))
    d.rounded_rectangle([sx0 + S * 0.03, sy0 + S * 0.03, sx1 - S * 0.03, sy1 - S * 0.03],
                        radius=int(S * 0.02), fill=(30, 64, 110, 255))
    # Stand.
    d.polygon([(S * 0.44, sy1), (S * 0.56, sy1), (S * 0.60, S * 0.72), (S * 0.40, S * 0.72)],
              fill=(238, 244, 250, 255))
    d.rounded_rectangle([S * 0.34, S * 0.72, S * 0.66, S * 0.755],
                        radius=int(S * 0.016), fill=(238, 244, 250, 255))

    # The remote pointer, in the screen.
    px, py = S * 0.60, S * 0.46
    d.polygon([(px, py), (px, py + S * 0.105), (px + S * 0.028, py + S * 0.078),
               (px + S * 0.058, py + S * 0.100)], fill=(255, 176, 46, 255))

    # "PPC" only where it can be read. Below about 128 it is a smear, and a
    # smear is worse than nothing.
    if size >= 128 and os.path.exists(FONT):
        from PIL import ImageFont
        f = ImageFont.truetype(FONT, int(S * 0.115))
        text = "PPC"
        bbox = d.textbbox((0, 0), text, font=f)
        d.text(((S - (bbox[2] - bbox[0])) / 2 - bbox[0], S * 0.79), text,
               font=f, fill=(238, 244, 250, 235))

    return img.resize((size, size), Image.LANCZOS)


# --------------------------------------------------------------------------
# ICNS RLE. Runs of 3..130 identical bytes become 0x80|(n-3) plus the byte;
# anything else becomes (n-1) followed by n literal bytes, n up to 128.
# --------------------------------------------------------------------------
def rle(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 130:
            run += 1
        if run >= 3:
            out += bytes([0x80 | (run - 3), data[i]])
            i += run
        else:
            start = i
            lit = bytearray()
            while i < n and len(lit) < 128:
                # Stop a literal when a run of three starts, so it can be encoded.
                if (i + 2 < n and data[i] == data[i + 1] == data[i + 2]):
                    break
                lit.append(data[i])
                i += 1
            if not lit:                       # safety: never loop without progress
                lit.append(data[i]); i += 1
            out += bytes([len(lit) - 1]) + bytes(lit)
            assert i > start
    return bytes(out)


def unrle(data, expected):
    """The decoder exists to check the encoder, not because anything reads it."""
    out = bytearray()
    i = 0
    while len(out) < expected:
        b = data[i]; i += 1
        if b & 0x80:
            out += bytes([data[i]]) * ((b & 0x7F) + 3); i += 1
        else:
            n = b + 1
            out += data[i:i + n]; i += n
    return bytes(out[:expected])


def rgb_entry(img, leading_zeros=False):
    px = img.convert("RGBA").tobytes()
    n = img.size[0] * img.size[1]
    planes = b""
    for ch in range(3):                       # R, G, B -- alpha lives in the mask
        plane = bytes(px[ch::4])
        packed = rle(plane)
        assert unrle(packed, n) == plane, "RLE round trip failed"
        planes += packed
    return (b"\x00\x00\x00\x00" if leading_zeros else b"") + planes


def mask_entry(img):
    return bytes(img.convert("RGBA").tobytes()[3::4])


def chunk(kind, payload):
    return kind + struct.pack(">I", len(payload) + 8) + payload


def main():
    import io
    entries = []
    for size, rgb_t, mask_t in ((16, b"is32", b"s8mk"),
                                (32, b"il32", b"l8mk"),
                                (128, b"it32", b"t8mk")):
        im = draw(size)
        entries.append(chunk(rgb_t, rgb_entry(im, leading_zeros=(size == 128))))
        entries.append(chunk(mask_t, mask_entry(im)))

    buf = io.BytesIO()
    draw(256).save(buf, format="PNG")
    entries.append(chunk(b"ic08", buf.getvalue()))

    body = b"".join(entries)
    with open(OUT, "wb") as f:
        f.write(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print("wrote %s (%d bytes, %d entries)" % (OUT, os.path.getsize(OUT), len(entries)))


if __name__ == "__main__":
    main()
