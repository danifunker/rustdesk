#!/usr/bin/env python3
"""A scriptable RustDesk peer for testing C-Desk-Vint without a GUI.

Logs in the way a real client does (empty-password probe, then
sha256(sha256(password + salt) + challenge)), keeps reading so the agent's
queue drains, and plays a script of input events:

    cdvpoke.py [--secure] [--files] HOST:PORT PASSWORD [STEP ...]

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
    zclip TEXT        the same, zstd-compressed, as a client sends all but short text
    save FILE         from now on, append every VP8 frame to FILE (4-byte
                      big-endian length, then the data) for host/build/vp8dump
    chat TEXT         a chat line to the Mac (Misc.chat_message)
    shot FILE         ask for a screenshot, wait for it, write the PNG to FILE
    wheel DY [DX]     a wheel event (DY > 0 up, DX > 0 left)
    quality N         image_quality: 2 Low, 3 Balanced, 4 Best
    fps N             custom_fps
    restart           ask the Mac to restart (Misc.restart_remote_device)

--files logs in as the client's file manager does (LoginRequest.file_transfer)
and takes these steps instead:
    ls PATH             list a folder
    get REMOTE LOCAL    download a file or folder
    put LOCAL REMOTE    upload a file (REMOTE: its full path on the agent)
    mkdir PATH / rm PATH / rmdir PATH / mv PATH NEWNAME

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


def unzz(v):
    return (v >> 1) ^ -(v & 1)


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
        self.shot = None
        self.files = []      # FileActions and FileResponses from the agent
        self.file_mode = False

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
            if 17 in m:
                self.files.append((17, parse(m[17][0])))
            if 18 in m:
                self.files.append((18, parse(m[18][0])))
            if 19 in m:  # misc: a chat line from the Mac
                mi = parse(m[19][0])
                if 4 in mi:
                    text = parse(mi[4][0]).get(1, [b''])[0]
                    print('chat from the Mac: %r' % text.decode('utf-8', 'replace'))
            if 30 in m:  # screenshot_response
                sr = parse(m[30][0])
                self.shot = (sr.get(3, [b''])[0], sr.get(2, [b''])[0].decode('utf-8', 'replace'),
                             sr.get(1, [b''])[0])
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
        ft = field(7, 2, field(1, 2, b'')) if self.file_mode else b''
        self.send(field(7, 2, field(2, 2, h2) + field(4, 2, b'poke') + field(5, 2, b'cdvpoke') +
                        ft + field(11, 2, b'1.4.9') + field(13, 2, b'Linux')))
        lr = parse(self.recv(10)[8][0])
        if 2 not in lr:
            raise SystemExit('login refused: %r' % lr.get(1))
        pi = parse(lr[2][0])
        if self.file_mode:
            print('logged in for files: %s, %s %s' % (pi[2][0].decode(), pi[3][0].decode(),
                                                      pi[7][0].decode()))
            return
        d = parse(pi[4][0])
        print('logged in: %s, %s %s, %dx%d' % (pi[2][0].decode(), pi[3][0].decode(),
                                              pi[7][0].decode(), d[3][0], d[4][0]))

    # ---- file manager ------------------------------------------------------------

    def file_wait(self, want, timeout=60):
        """The next FileResponse/FileAction of kind (field, union) in `want`."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            while self.files:
                fld, m = self.files.pop(0)
                for k in m:
                    if (fld, k) in want:
                        return fld, k, parse(m[k][0])
                    if (fld, k) == (18, 3):  # error
                        e = parse(m[k][0])
                        print('  error: %s (file %d)' % (
                            e.get(2, [b''])[0].decode('utf-8', 'replace'),
                            unzz(e.get(3, [0])[0])))
                        if (18, 3) in want:
                            return fld, k, e
            self.pump(0.2)
        raise SystemExit('no answer in %d s' % timeout)

    def ls(self, path):
        self.send(field(17, 2, field(1, 2, field(1, 2, path.encode()))))
        _, _, d = self.file_wait({(18, 1), (18, 3)})
        print('%s:' % d.get(2, [b'?'])[0].decode('utf-8', 'replace'))
        for raw in d.get(3, []):
            e = parse(raw)
            kind = {0: 'dir ', 3: 'disk', 4: 'file'}.get(e.get(1, [0])[0], '?')
            print('  %s %10d  %s%s' % (kind, e.get(4, [0])[0],
                                      e.get(2, [b''])[0].decode('utf-8', 'replace'),
                                      ' (hidden)' if e.get(3, [0])[0] else ''))

    def get(self, remote, local):
        import os
        job = 7
        self.send(field(17, 2, field(2, 2, field(1, 0, job) + field(2, 2, remote.encode()))))
        _, _, d = self.file_wait({(18, 1), (18, 3)})
        names = [parse(r).get(2, [b''])[0].decode() for r in d.get(3, [])]
        print('  %d file(s)' % len(names))
        out, cur, total = None, None, 0
        t0 = time.time()
        while True:
            fld, k, m = self.file_wait({(18, 2), (18, 4), (18, 3)})
            if k == 4:
                break
            if k == 3:
                continue
            num = unzz(m.get(2, [0])[0])
            if num != cur:
                if out:
                    out.close()
                name = names[num]
                path = os.path.join(local, name) if name else local
                os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
                out, cur = open(path, 'wb'), num
                print('  %s' % path)
            data = m.get(3, [b''])[0]
            if m.get(4, [0])[0]:
                import zstandard
                data = zstandard.ZstdDecompressor().decompress(data)
            out.write(data)
            total += len(data)
        if out:
            out.close()
        dt = time.time() - t0
        print('  done: %d bytes in %.1f s (%.0f KB/s)' % (total, dt, total / 1024 / max(dt, 0.01)))

    def put(self, local, remote):
        import os
        import zstandard
        job = 9
        data = open(local, 'rb').read()
        mtime = int(os.path.getmtime(local))
        entry = field(1, 0, 4) + field(2, 2, b'') + field(4, 0, len(data)) + field(5, 0, mtime)
        self.send(field(17, 2, field(3, 2, field(1, 0, job) + field(2, 2, remote.encode()) +
                                     field(3, 2, entry) + field(5, 0, len(data)))))
        # The digest, as a 1.4.9 client sends it, and wait for the go-ahead.
        self.send(field(18, 2, field(5, 2, field(1, 0, job) + field(2, 0, zz(0)) +
                                     field(3, 0, mtime) + field(4, 0, len(data)) +
                                     field(5, 0, 1))))
        fld, k, m = self.file_wait({(17, 9), (18, 3)})
        if k != 9:
            return
        print('  confirmed: %r' % {kk: v[0] for kk, v in m.items()})
        t0 = time.time()
        off = 0
        while True:
            chunk = data[off:off + 128 * 1024]
            z = zstandard.ZstdCompressor(level=3).compress(chunk)
            comp = len(z) < len(chunk)
            blk = field(1, 0, job) + field(2, 0, zz(0)) + field(3, 2, z if comp else chunk)
            if comp:
                blk += field(4, 0, 1)
            self.send(field(18, 2, field(2, 2, blk)))
            off += len(chunk)
            if off >= len(data):
                break
        self.send(field(18, 2, field(4, 2, field(1, 0, job) + field(2, 0, zz(0)))))
        fld, k, m = self.file_wait({(18, 4), (18, 3)})
        dt = time.time() - t0
        print('  %s: %d bytes in %.1f s' % ('done' if k == 4 else 'failed', len(data), dt))

    def simple(self, union, body, file_num_field=None):
        job = 11
        self.send(field(17, 2, field(union, 2, field(1, 0, job) + body)))
        fld, k, m = self.file_wait({(18, 4), (18, 3)})
        print('  %s' % ('done' if k == 4 else 'failed'))

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
    secure = '--secure' in argv
    file_mode = '--files' in argv
    argv = [a for a in argv if a not in ('--secure', '--files')]
    p = Peer(argv[0])
    p.file_mode = file_mode
    if secure:
        p.handshake()
    p.login(argv[1])
    if file_mode:
        # The agent lists the folder the client asked to start in, unasked.
        p.file_wait({(18, 1)})
        args = argv[2:]
        i = 0
        while i < len(args):
            op = args[i]
            i += 1
            print('%s %s' % (op, ' '.join(args[i:i + (2 if op in ('get', 'put', 'mv') else 1)])))
            if op == 'ls':
                p.ls(args[i]); i += 1
            elif op == 'get':
                p.get(args[i], args[i + 1]); i += 2
            elif op == 'put':
                p.put(args[i], args[i + 1]); i += 2
            elif op == 'mkdir':
                p.simple(4, field(2, 2, args[i].encode())); i += 1
            elif op == 'rm':
                p.simple(6, field(2, 2, args[i].encode())); i += 1
            elif op == 'rmdir':
                p.simple(5, field(2, 2, args[i].encode()) + field(3, 0, 1)); i += 1
            elif op == 'mv':
                p.simple(10, field(2, 2, args[i].encode()) + field(3, 2, args[i + 1].encode()))
                i += 2
            elif op == 'sleep':
                p.pump(float(args[i])); i += 1
            else:
                raise SystemExit('unknown step ' + op)
        return
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
        elif op == 'zclip':
            import zstandard
            z = zstandard.ZstdCompressor(level=3).compress(args[i].encode('utf-8'))
            p.send(field(16, 2, field(1, 0, 1) + field(2, 2, z)))
            i += 1
        elif op == 'chat':
            p.send(field(19, 2, field(4, 2, field(1, 2, args[i].encode('utf-8')))))
            i += 1
        elif op == 'shot':
            p.shot = None
            p.send(field(29, 2, field(1, 0, 0) + field(2, 2, b'cdvpoke')))
            t0 = time.time()
            while p.shot is None and time.time() - t0 < 60:
                p.pump(0.5)
            if p.shot is None:
                print('no screenshot in 60 s')
            else:
                data, msg, sid = p.shot
                print('screenshot: %d bytes, sid %r, %s, %.1f s' % (
                    len(data), sid.decode('utf-8', 'replace'), msg or 'no error', time.time() - t0))
                if data:
                    open(args[i], 'wb').write(data)
            i += 1
        elif op == 'wheel':
            dy = int(args[i])
            i += 1
            dx = 0
            if i < len(args) and args[i].lstrip('-').isdigit():
                dx = int(args[i])
                i += 1
            p.mouse(3, dx, dy)
        elif op in ('quality', 'fps'):
            num = 1 if op == 'quality' else 11
            p.send(field(19, 2, field(7, 2, field(num, 0, int(args[i])))))
            i += 1
        elif op == 'restart':
            p.send(field(19, 2, field(14, 0, 1)))
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
