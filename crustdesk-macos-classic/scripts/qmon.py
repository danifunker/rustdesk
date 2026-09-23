#!/usr/bin/env python3
"""A QEMU monitor client: qmon.py SOCKET CMD [CMD ...].

'sleep S' waits; 'shot PATH' saves a PNG screendump to PATH.png."""
import socket
import sys
import time

s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.settimeout(2)


def drain():
    b = b''
    try:
        while not b.endswith(b'(qemu) '):
            d = s.recv(65536)
            if not d:
                break
            b += d
    except socket.timeout:
        pass


drain()
for c in sys.argv[2:]:
    if c.startswith('sleep '):
        time.sleep(float(c.split()[1]))
        continue
    if c.startswith('shot '):
        c = 'screendump %s.png -f png' % c.split(None, 1)[1]
    s.sendall((c + '\n').encode())
    drain()
