#!/usr/bin/env python3
"""Drive the NT test machine (scripts/qemu-40p.sh) over QMP: its screen and
keyboard, before tools/nt/ntexec.c is running there -- ntexec.c goes in on
a CD (genisoimage) and is built by typing at a command prompt.

  ntvm.py shot OUT.png            screendump -> png
  ntvm.py keys ctrl-alt-delete    send key combos (space separated)
  ntvm.py type 'text\n'           type text (\n = ret)
  ntvm.py cd FILE.iso | eject     change / eject cd0
  ntvm.py hmp 'info status'       raw HMP command
"""
import json, socket, sys, time, os

SOCK = os.path.join(os.environ.get("NTVM", "/run/media/dani/BigDisk/winnt-ppc"), "run", "qmp.sock")

class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(SOCK)
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}).encode() + b"\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                if "error" in r:
                    raise SystemExit(r["error"])
                return r["return"]

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
           '*': '8', '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
           '}': 'bracket_right', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
           '<': 'comma', '>': 'dot', '?': 'slash', '~': 'grave_accent'}
PLAIN = {' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '=': 'equal', '[': 'bracket_left',
         ']': 'bracket_right', '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe',
         ',': 'comma', '.': 'dot', '/': 'slash', '`': 'grave_accent'}

def keyname(c):
    if c.isalpha():
        return ("shift-" + c.lower()) if c.isupper() else c
    if c.isdigit():
        return c
    if c in PLAIN:
        return PLAIN[c]
    if c in SHIFTED:
        return "shift-" + SHIFTED[c]
    raise ValueError(repr(c))

def main():
    q = QMP()
    op = sys.argv[1]
    if op == "shot":
        out = sys.argv[2]
        tmp = out + ".ppm"
        q.cmd("screendump", filename=os.path.abspath(tmp))
        from PIL import Image
        Image.open(tmp).save(out)
        os.unlink(tmp)
    elif op == "keys":
        for k in sys.argv[2:]:
            q.hmp("sendkey " + k)
            time.sleep(0.08)
    elif op == "type":
        text = sys.argv[2].encode().decode("unicode_escape")
        for c in text:
            q.hmp("sendkey " + keyname(c))
            time.sleep(0.03)
    elif op == "cd":
        if sys.argv[2] == "eject":
            q.hmp("eject -f cd0")
        else:
            q.hmp("change cd0 " + os.path.abspath(sys.argv[2]))
    elif op == "hmp":
        print(q.hmp(" ".join(sys.argv[2:])))

if __name__ == "__main__":
    main()
