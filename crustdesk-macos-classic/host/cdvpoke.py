#!/usr/bin/env python3
"""A scriptable RustDesk peer for testing C-Desk-Vint without a GUI.

Logs in the way a real client does (empty-password probe, then
sha256(sha256(password + salt) + challenge)), keeps reading so the agent's
queue drains, and plays a script of input events:

    cdvpoke.py [--secure] HOST:PORT PASSWORD [STEP ...]

--secure plays the client's side of the key exchange a peer arriving through
the ID server does (signed_id, then public_key), and seals everything after.

Steps:
    move X Y          pointer to X,Y
    down [X Y]        left button down (at X,Y, or where the pointer is)
    up [X Y]          left button up
    rdown / rup       the same for the right button
    key CODE          a Mac virtual keycode, pressed and released (Map mode)
    keydown CODE / keyup CODE
    type TEXT         characters, Legacy mode (chr is a character)
    sleep SECONDS
    refresh           ask for a keyframe
    frames SECONDS    just watch, and report what arrives
    clip TEXT         put TEXT on the Mac's clipboard (Clipboard, uncompressed)
    save FILE         from now on, append every VP8 frame to FILE (4-byte
                      big-endian length, then the data) for host/build/vp8dump

Only the handful of protobuf fields involved are encoded, by hand.
"""
import hashlib
import socket
import struct
import sys
import time


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def field(num, wire, payload):
    tag = varint(num << 3 | wire)
    if wire == 0:
        return tag + varint(payload)
    return tag + varint(len(payload)) + payload


def zz(v):
    return (v << 1) ^ (v >> 31)


def frame(body):
    n = len(body)
    if n <= 0x3F:
        h = struct.pack('<B', n << 2)
    elif n <= 0x3FFF:
        h = struct.pack('<H', n << 2 | 1)
    elif n <= 0x3FFFFF:
        v = n << 2 | 2
        h = bytes([v & 255, v >> 8 & 255, v >> 16 & 255])
    else:
        h = struct.pack('<I', n << 2 | 3)
    return h + body


def parse(buf):
    """Top-level fields of a protobuf message: {field: [values]}."""
    out, i = {}, 0
    while i < len(buf):
        key, i = rvarint(buf, i)
        f, w = key >> 3, key & 7
        if w == 0:
            v, i = rvarint(buf, i)
        elif w == 2:
            n, i = rvarint(buf, i)
            v = buf[i:i + n]
            i += n
        elif w == 5:
            v = buf[i:i + 4]
            i += 4
        elif w == 1:
            v = buf[i:i + 8]
            i += 8
        else:
            raise ValueError('wire type %d' % w)
        out.setdefault(f, []).append(v)
    return out


def rvarint(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return v, i


class Peer:
    key = None
    send_seq = recv_seq = 0

    def __init__(self, addr):
        host, port = addr.rsplit(':', 1)
        self.s = socket.create_connection((host, int(port)), timeout=10)
        self.buf = b''
        self.frames = self.keyframes = self.bytes = 0
        self.save = None
        self.cursors, self.positions = [], []
        self.box_key = None  # set by the secure handshake

    def send(self, body):
        if self.box_key:
            from nacl.bindings import crypto_secretbox
            self.send_seq += 1
            body = crypto_secretbox(body, self.send_seq.to_bytes(8, 'little') + bytes(16), self.box_key)
        self.s.sendall(frame(body))

    def handshake(self):
        """What a client does with signed_id: check the signature, take the
        box key out of IdPk, and box a fresh session key to it."""
        from nacl.bindings import (crypto_box, crypto_box_keypair, crypto_sign_open)
        m = self.recv(15, raw=True)
        signed = parse(parse(m)[3][0])[1][0]
        idpk = parse(signed[64:])
        their = idpk[2][0]
        print('signed_id from %s, box key %s...' % (idpk[1][0].decode(), their[:4].hex()))
        pk, sk = crypto_box_keypair()
        key = bytes(range(32))
        boxed = crypto_box(key, bytes(24), their, sk)
        self.s.sendall(frame(field(4, 2, field(1, 2, pk) + field(2, 2, boxed))))
        self.box_key = key

    def recv(self, timeout=None, raw=False):
        self.s.settimeout(timeout)
        while True:
            if self.buf:
                hl = (self.buf[0] & 3) + 1
                if len(self.buf) >= hl:
                    n = int.from_bytes(self.buf[:hl], 'little') >> 2
                    if len(self.buf) >= hl + n:
                        body = self.buf[hl:hl + n]
                        self.buf = self.buf[hl + n:]
                        if raw:
                            return body
                        if self.box_key:
                            from nacl.bindings import crypto_secretbox_open
                            self.recv_seq += 1
                            body = crypto_secretbox_open(
                                body, self.recv_seq.to_bytes(8, 'little') + bytes(16), self.box_key)
                        return parse(body)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return None
            if not d:
                raise EOFError('agent closed the connection')
            self.buf += d

    def pump(self, seconds):
        """Read and account for everything that arrives for a while."""
        end = time.time() + seconds
        while time.time() < end:
            m = self.recv(max(0.01, end - time.time()))
            if m is None:
                continue
            if 6 in m:  # video_frame
                self.frames += 1
                vf = parse(m[6][0])
                enc = parse((vf.get(12) or vf.get(6))[0])
                one = parse(enc[1][0])
                self.bytes += len(one[1][0])
                if self.save:
                    self.save.write(struct.pack('>I', len(one[1][0])) + one[1][0])
                if one.get(2, [0])[0]:
                    self.keyframes += 1
            for f in (16, 28):  # clipboard, multi_clipboards
                if f in m:
                    c = parse(m[f][0]) if f == 16 else parse(parse(m[f][0])[1][0])
                    text = c.get(2, [b''])[0]
                    print('clipboard from the Mac (%s, %s): %r' % (
                        'multi' if f == 28 else 'single',
                        'compressed' if c.get(1, [0])[0] else 'plain', text.decode('utf-8', 'replace')))
            if 12 in m:  # cursor_data
                cd = parse(m[12][0])
                self.cursors.append(cd)
            if 13 in m:  # cursor_position
                cp = parse(m[13][0])
                un = lambda v: (v >> 1) ^ -(v & 1)
                self.positions.append((un(cp.get(1, [0])[0]), un(cp.get(2, [0])[0])))
            if 5 in m:  # test_delay: answer it as a client does
                td = parse(m[5][0])
                if not td.get(2, [0])[0]:
                    self.send(field(5, 2, m[5][0]))

    def login(self, password):
        h = parse(self.recv(10)[9][0])
        salt, challenge = h[1][0], h[2][0]
        self.send(field(7, 2, field(4, 2, b'poke') + field(5, 2, b'cdvpoke')))
        m = self.recv(10)
        err = parse(m[8][0]).get(1, [b''])[0]
        assert err == b'Empty Password', err
        h1 = hashlib.sha256(password.encode() + salt).digest()
        h2 = hashlib.sha256(h1 + challenge).digest()
        self.send(field(7, 2, field(2, 2, h2) + field(4, 2, b'poke') + field(5, 2, b'cdvpoke') +
                        field(11, 2, b'1.4.9') + field(13, 2, b'Linux')))
        lr = parse(self.recv(10)[8][0])
        if 2 not in lr:
            raise SystemExit('login refused: %r' % lr.get(1))
        pi = parse(lr[2][0])
        d = parse(pi[4][0])
        print('logged in: %s, %s %s, %dx%d' % (pi[2][0].decode(), pi[3][0].decode(),
                                              pi[7][0].decode(), d[3][0], d[4][0]))

    def mouse(self, mask, x=0, y=0):
        body = field(1, 0, mask)
        if x:
            body += field(2, 0, zz(x))
        if y:
            body += field(3, 0, zz(y))
        self.send(field(10, 2, body))

    def key(self, code, down=None, press=False, mode=1):
        body = b''
        if down:
            body += field(1, 0, 1)
        if press:
            body += field(2, 0, 1)
        body += field(4, 0, code)
        if mode:
            body += field(9, 0, mode)
        self.send(field(15, 2, body))


def main():
    argv = sys.argv[1:]
    secure = argv[0] == '--secure'
    if secure:
        argv = argv[1:]
    p = Peer(argv[0])
    if secure:
        p.handshake()
    p.login(argv[1])
    args = argv[2:]
    i = 0

    def xy():
        nonlocal i
        if i + 1 < len(args) and args[i].lstrip('-').isdigit():
            x, y = int(args[i]), int(args[i + 1])
            i += 2
            return x, y
        return 0, 0

    while i < len(args):
        op = args[i]
        i += 1
        if op == 'move':
            p.mouse(0, *xy())
        elif op in ('down', 'up', 'rdown', 'rup'):
            btn = 2 if op.startswith('r') else 1
            p.mouse((1 if op.endswith('down') else 2) | btn << 3, *xy())
        elif op == 'key':
            p.key(int(args[i]), press=True)
            i += 1
        elif op in ('keydown', 'keyup'):
            p.key(int(args[i]), down=op == 'keydown')
            i += 1
        elif op == 'type':
            for ch in args[i]:
                p.key(ord(ch), down=True, mode=0)
                p.key(ord(ch), down=False, mode=0)
            i += 1
        elif op == 'sleep':
            p.pump(float(args[i]))
            i += 1
        elif op == 'clip':
            p.send(field(16, 2, field(2, 2, args[i].encode('utf-8'))))
            i += 1
        elif op == 'save':
            p.save = open(args[i], 'wb')
            i += 1
        elif op == 'refresh':
            p.send(field(19, 2, field(10, 0, 1)))
        elif op == 'frames':
            f0, k0, b0 = p.frames, p.keyframes, p.bytes
            t = float(args[i])
            i += 1
            p.pump(t)
            print('%d frames (%d key), %d bytes in %.1fs' %
                  (p.frames - f0, p.keyframes - k0, p.bytes - b0, t))
        else:
            raise SystemExit('unknown step %s' % op)
        p.pump(0.05)
    print('total: %d frames (%d key), %d bytes' % (p.frames, p.keyframes, p.bytes))
    if p.cursors or p.positions:
        print('cursor shapes: %d, positions: %s' % (len(p.cursors), p.positions[-3:]))
        if p.cursors:
            cd = p.cursors[-1]
            open('/tmp/cdv-cursor.zst', 'wb').write(cd[6][0])
            un = lambda v: (v >> 1) ^ -(v & 1)
            print('last shape: %dx%d, hot %d,%d, %d bytes of zstd' % (
                cd[4][0], cd[5][0], un(cd.get(2, [0])[0]), un(cd.get(3, [0])[0]), len(cd[6][0])))


if __name__ == '__main__':
    main()
