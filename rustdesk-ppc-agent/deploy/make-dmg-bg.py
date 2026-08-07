#!/usr/bin/env python3
"""Draw the disk-image background and write deploy/dmg-background.png.

Run when the artwork changes; the PNG is committed, so building a release needs
neither Python nor PIL.

The size here and the window bounds in deploy/make-dmg.sh are the same numbers
and have to stay that way: the Finder scales nothing, so a mismatch shows as the
arrow pointing at empty space rather than as an error.
"""
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("this needs Pillow: pip install Pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "dmg-background.png")
W, H = 600, 400                      # must match WIN_W/WIN_H in make-dmg.sh
ICON_Y = 200                         # must match the icon positions there
APP_X, DEST_X = 150, 450

FONT_R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def main():
    img = Image.new("RGB", (W, H), (247, 249, 252))
    d = ImageDraw.Draw(img)

    # A soft wash, lighter at the top, so the icons sit on something.
    for y in range(H):
        t = y / float(H)
        c = (int(247 - 14 * t), int(249 - 12 * t), int(252 - 8 * t))
        d.line([(0, y), (W, y)], fill=c)

    if os.path.exists(FONT_B):
        fb = ImageFont.truetype(FONT_B, 19)
        fr = ImageFont.truetype(FONT_R, 13)
        title = "Install Agent for RustDesk PPC"
        sub = "Drag the app onto the Applications folder."
        for text, font, y, fill in ((title, fb, 42, (28, 54, 92)),
                                    (sub, fr, 74, (96, 110, 130))):
            bb = d.textbbox((0, 0), text, font=font)
            d.text(((W - (bb[2] - bb[0])) / 2 - bb[0], y), text, font=font, fill=fill)

        note = "The app installs the background service and is where its settings live."
        bb = d.textbbox((0, 0), note, font=fr)
        d.text(((W - (bb[2] - bb[0])) / 2 - bb[0], H - 52), note,
               font=fr, fill=(130, 142, 160))

    # The arrow, between the two icon slots rather than through them: icons are
    # 96px, so it starts and ends clear of both.
    y = ICON_Y + 6
    x0, x1 = APP_X + 74, DEST_X - 74
    d.line([(x0, y), (x1 - 16, y)], fill=(150, 168, 190), width=5)
    d.polygon([(x1, y), (x1 - 20, y - 12), (x1 - 20, y + 12)], fill=(150, 168, 190))

    img.save(OUT)
    print("wrote %s (%dx%d)" % (OUT, W, H))


if __name__ == "__main__":
    main()
