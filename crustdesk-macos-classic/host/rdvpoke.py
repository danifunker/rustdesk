#!/usr/bin/env python3
"""cdvpoke.py, reaching the agent by its ID through an ID server, the way a
client on the internet does -- rather than by its address.

    rdvpoke.py [--server HOST[:PORT]] [--key KEY] [--relay | --local] ID PASSWORD [STEP ...]

The client asks hbbs for the ID (PunchHoleRequest). What comes back decides
the way in:

  - a RelayResponse: the agent has gone to hbbr with a uuid; join it there.
  - a PunchHoleResponse with is_local: hbbs saw us on the agent's network
    and the agent is listening at the local address it gave (--local
    connects there; QEMU's user networking needs a forward for that port,
    which --forward CMD runs with the port number appended).
  - otherwise, or with --relay: do what a client does when the direct way
    fails -- RequestRelay to hbbs with a uuid of our own, and meet the agent
    at the relay.

The session itself is then cdvpoke.py's, with the key exchange (--secure):
STEPs are cdvpoke's. Default server rs-ny.rustdesk.com and its public key.
"""
import os
import socket
import subprocess
import sys
import uuid as uuidlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdvpoke  # noqa: E402
from cdvpoke import field, frame, parse  # noqa: E402

PUBLIC_KEY = 'OeVuKk5nlHiXp+APNn0Y3pC1Iwpwn44JGqrQCsWqmBw='


def unmangle(b):
    """hbbs's socket-address obfuscation, undone (rdv.c rdv_unmangle)."""
    b = b + bytes(16 - len(b))
    lo = int.from_bytes(b[:8], 'little')
    hi = int.from_bytes(b[8:], 'little')
    tm = (lo >> 17) & 0xFFFFFFFF
    a = ((lo >> 49) | (hi << 15)) & 0xFFFFFFFFFFFFFFFF
    ip_le = (a - tm) & 0xFFFFFFFF
    ip = socket.inet_ntoa(ip_le.to_bytes(4, 'little'))
    port = ((lo & 0xFFFFFF) - (tm & 0xFFFF)) & 0xFFFF
    return ip, port


class Conn:
    """A framed TCP connection to hbbs, sealed once secure_tcp has run."""

    def __init__(self, s):
        self.s, self.buf, self.key = s, b'', None
        self.send_seq = self.recv_seq = 0

    def send(self, body):
        if self.key:
            from nacl.bindings import crypto_secretbox
            self.send_seq += 1
            body = crypto_secretbox(body, self.send_seq.to_bytes(8, 'little') + bytes(16), self.key)
        self.s.sendall(frame(body))

    def recv(self, timeout=20):
        self.s.settimeout(timeout)
        while True:
            if self.buf:
                hl = (self.buf[0] & 3) + 1
                if len(self.buf) >= hl:
                    n = int.from_bytes(self.buf[:hl], 'little') >> 2
                    if len(self.buf) >= hl + n:
                        body, self.buf = self.buf[hl:hl + n], self.buf[hl + n:]
                        if self.key:
                            from nacl.bindings import crypto_secretbox_open
                            self.recv_seq += 1
                            body = crypto_secretbox_open(
                                body, self.recv_seq.to_bytes(8, 'little') + bytes(16), self.key)
                        return parse(body)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return None
            if not d:
                raise EOFError('the server closed the connection')
            self.buf += d

    def secure(self, server_key):
        """The client's secure_tcp: a server with a key opens with
        KeyExchange{its box key, signed}; answer with ours and a session key
        boxed to it, and seal everything after. An old server says nothing."""
        import base64
        from nacl.bindings import crypto_box, crypto_box_keypair, crypto_sign_open
        m = self.recv(timeout=3)
        if m is None:
            print('hbbs: no key exchange (an old server); in the clear')
            return
        if 25 not in m:
            raise SystemExit('hbbs opened with %s, not a key exchange' % sorted(m))
        signed = parse(m[25][0])[1][0]
        their = crypto_sign_open(signed, base64.b64decode(server_key))
        pk, sk = crypto_box_keypair()
        key = os.urandom(32)
        boxed = crypto_box(key, bytes(24), their, sk)
        self.send(field(25, 2, field(1, 2, pk) + field(1, 2, boxed)))
        self.key = key
        print('hbbs: key exchange done')


def split(host, port):
    if ':' in host:
        h, p = host.rsplit(':', 1)
        return h, int(p)
    return host, port


def relay_join(relay, uid, key):
    host, port = split(relay, 21117)
    print('joining the relay %s:%d, uuid %s' % (host, port, uid))
    r = socket.create_connection((host, port), timeout=15)
    req = field(2, 2, uid.encode()) + field(6, 2, key.encode())
    r.sendall(frame(field(18, 2, req)))
    return r


def main():
    argv = sys.argv[1:]
    server, key, mode, forward = 'rs-ny.rustdesk.com', PUBLIC_KEY, None, None
    while argv and argv[0].startswith('--'):
        opt = argv.pop(0)
        if opt == '--server':
            server = argv.pop(0)
        elif opt == '--key':
            key = argv.pop(0)
        elif opt == '--forward':
            forward = argv.pop(0)
        elif opt in ('--relay', '--local'):
            mode = opt[2:]
        else:
            raise SystemExit(__doc__)
    if len(argv) < 2:
        raise SystemExit(__doc__)
    peer_id, password, steps = argv[0], argv[1], argv[2:]
    host, port = split(server, 21116)
    s = Conn(socket.create_connection((host, port), timeout=20))
    s.secure(key)
    # PunchHoleRequest: id, nat_type SYMMETRIC (we cannot be punched to),
    # licence_key, version, force_relay
    ph = (field(1, 2, peer_id.encode()) + field(2, 0, 2) + field(3, 2, key.encode()) +
          field(6, 2, b'1.4.5') + field(8, 0, 1))
    s.send(field(8, 2, ph))
    m = s.recv()
    if m is None:
        raise SystemExit('hbbs did not answer')
    sock = None
    relay_server = ''
    if 19 in m:
        rr = parse(m[19][0])
        uid = rr.get(2, [b''])[0].decode()
        relay_server = rr.get(3, [b''])[0].decode()
        print('hbbs: the agent is at the relay %s' % relay_server)
        sock = relay_join(relay_server, uid, key)
    elif 11 in m:
        r = parse(m[11][0])
        if r.get(3) or r.get(7):
            raise SystemExit('hbbs: failure %s %s' % (r.get(3, [0])[0], r.get(7, [b''])[0]))
        relay_server = r.get(4, [b''])[0].decode()
        local = r.get(6, [0])[0]
        addr = unmangle(r[1][0]) if r.get(1) else None
        print('hbbs: punch hole response, %s %s, relay %s' % (
            'local' if local else 'remote', addr, relay_server))
        if mode == 'local' and addr:
            if forward:
                subprocess.run(forward.split() + [str(addr[1])], check=True)
                addr = ('127.0.0.1', addr[1])
            print('connecting to %s:%d' % addr)
            sock = socket.create_connection(addr, timeout=10)
    else:
        raise SystemExit('hbbs answered %s' % sorted(m))
    if sock is None:
        # The client's fallback: a relay of our choosing, through hbbs.
        uid = str(uuidlib.uuid4())
        rq = (field(1, 2, peer_id.encode()) + field(2, 2, uid.encode()) +
              field(4, 2, relay_server.encode()) + field(5, 0, 1) + field(6, 2, key.encode()))
        s.send(field(18, 2, rq))
        print('asked hbbs to send the agent to %s, uuid %s' % (relay_server, uid))
        sock = relay_join(relay_server or host, uid, key)
    s.s.close()
    sock.settimeout(10)
    cdvpoke.main(['(%s)' % peer_id, password, '--secure'] + steps, sock=sock)


if __name__ == '__main__':
    main()
