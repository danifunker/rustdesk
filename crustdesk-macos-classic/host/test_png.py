#!/usr/bin/env python3
"""Decode the PNGs test_core wrote with PIL and compare them with the raw
pixels written beside them: proof the deflate and the framing are right."""
import sys
from PIL import Image

ok = True
for name, mode in (('t8', 'P'), ('t24', 'RGB')):
    im = Image.open('build/%s.png' % name)
    im.load()
    raw = open('build/%s.raw' % name, 'rb').read()
    got = im.tobytes() if im.mode == mode else None
    if im.size != (301, 203) or got != raw:
        print('FAIL: %s.png decodes to something else (%s, %s)' % (name, im.mode, im.size))
        ok = False
if ok:
    import os
    print('PASS: png (%d and %d bytes for 61 KB and 183 KB of pixels)' % (
        os.path.getsize('build/t8.png'), os.path.getsize('build/t24.png')))
sys.exit(0 if ok else 1)
