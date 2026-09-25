#!/usr/bin/env python3
"""Look for strings in built files, including where the 68k compiler hid them.

    find-strings.py [--must S]... [--must-not S]... FILE...

A plain `grep -a` misses a string the 68k half copies with strcpy: GCC
inlines the copy as `move.l #'rust',prefs+0x78` and so on, so the bytes are
there, but four at a time with an opcode and an address between each four.
Every string is looked for both ways: whole, and as a run of 4-byte pieces
(at each of the four alignments it could have inside a longer string) with at
most 12 other bytes between one piece and the next. The last piece may be
cut short, since the copy of a string's tail can be a move.w or move.b.

--must: each FILE must contain it (the public ID server, say -- which also
proves the scan still sees what the compiler does to a strcpy).
--must-not: no FILE may contain it (a personal hostname or key).

Exits 1 on any failure, naming the file and the string, never printing a
--must-not string itself (it may be a secret): only its position in the list.
"""
import re
import sys

GAP = rb'[\x00-\xff]{0,12}'


def pattern(s):
    s = s.encode('latin-1')
    alts = [re.escape(s)]
    for k in range(4):
        head, body = s[:k], s[k:]
        chunks = [body[i:i + 4] for i in range(0, len(body), 4)]
        if len(chunks) < 2:
            continue
        tail = chunks.pop() if len(chunks[-1]) < 4 else b''
        # A partial head ends the previous piece, so it sits right before a gap.
        parts = [re.escape(head) + GAP] if head else []
        parts.append(GAP.join(re.escape(c) for c in chunks))
        if tail:
            parts.append(GAP + re.escape(tail[:1]))
        alts.append(b''.join(parts))
    return re.compile(b'|'.join(b'(?:' + a + b')' for a in alts))


def main():
    must, must_not, files = [], [], []
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == '--must':
            must.append(args.pop(0))
        elif a == '--must-not':
            s = args.pop(0)
            if s:
                must_not.append(s)
        else:
            files.append(a)
    if not files:
        sys.exit(__doc__)
    bad = 0
    pm = [(s, pattern(s)) for s in must]
    pn = [pattern(s) for s in must_not]
    for f in files:
        data = open(f, 'rb').read()
        for s, p in pm:
            if not p.search(data):
                print('%s: does not contain %r' % (f, s))
                bad = 1
        for i, p in enumerate(pn):
            if p.search(data):
                print('%s: contains forbidden string #%d' % (f, i + 1))
                bad = 1
    if not bad:
        print('checked %d file(s): %d required, %d forbidden, all good'
              % (len(files), len(must), len(must_not)))
    sys.exit(bad)


if __name__ == '__main__':
    main()
