#!/usr/bin/env python3
"""The core's terminal and tunnel connections, against hostagent.

    test_connkinds.py PORT PASSWORD [--secure]

A terminal: features.terminal in PeerInfo, open, the agent's prompt, typed
keys answered. A tunnel: after the login the connection is raw TCP in both
directions, unframed and unencrypted, as upstream's set_raw() has it. A refused
tunnel target: the login fails with the agent's message. Each is a connection
of its own; hostagent serves one at a time.
"""
import sys
import time

import cdvpoke
from cdvpoke import Peer, field, parse


def peer(addr, secure, extra):
    p = Peer(addr)
    p.login_extra = extra
    if secure:
        p.handshake()
    return p


def term_message(p, want, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        m = p.recv(max(0.05, end - time.time()))
        if m and 32 in m:
            r = parse(m[32][0])
            if want in r:
                return parse(r[want][0])
    raise SystemExit('no terminal response %d' % want)


def terminal(addr, password, secure):
    p = peer(addr, secure, field(16, 2, field(1, 2, b'svc-1')))
    p.login(password)
    assert 9 in p.peer_info and parse(p.peer_info[9][0]).get(2) == [1], 'no features.terminal'
    assert 4 not in p.peer_info, 'a terminal has no display'
    p.send(field(31, 2, field(1, 2, field(1, 0, 3) + field(2, 0, 25) + field(3, 0, 80))))
    o = term_message(p, 1)
    assert o[1] == [3] and o[2] == [1] and o[4] == [4242] and o[5] == [b'host-term'], o
    d = term_message(p, 2)
    assert d[2] == [b'host> '], d
    p.send(field(31, 2, field(2, 2, field(1, 0, 3) + field(2, 2, b'dir\r'))))
    d = term_message(p, 2)
    assert d[1] == [3] and d[2] == [b'DIR\r'], d
    p.send(field(31, 2, field(4, 2, field(1, 0, 3))))
    term_message(p, 3)
    p.s.close()
    print('terminal: ok')


def tunnel(addr, password, secure):
    p = peer(addr, secure, field(8, 2, field(1, 2, b'localhost') + field(2, 0, 21)))
    p.login(password)
    assert p.buf == b'', 'bytes after the login response'
    for text in (b'hello', b'\x00\x01binary\xff', b'x' * 200):
        p.s.sendall(text)
        p.s.settimeout(5)
        got = b''
        while len(got) < len(text) + 4:
            got += p.s.recv(4096)
        assert got == b'tun:' + text, got
    p.s.close()
    print('tunnel: ok')


def refused(addr, password, secure):
    p = peer(addr, secure, field(8, 2, field(1, 2, b'refuse') + field(2, 0, 21)))
    try:
        p.login(password)
    except SystemExit as e:
        assert 'remote address' in str(e), e
        p.s.close()
        print('refused tunnel: ok')
        return
    raise SystemExit('a refused tunnel logged in')


def main():
    addr = '127.0.0.1:' + sys.argv[1]
    password = sys.argv[2]
    secure = '--secure' in sys.argv
    for test in (terminal, tunnel, refused):
        test(addr, password, secure)
        time.sleep(0.4)
    print('all passed' + (' (encrypted)' if secure else ''))


if __name__ == '__main__':
    main()
