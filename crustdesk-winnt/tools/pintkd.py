#!/usr/bin/env python3
"""pintkd: a host for Windows NT 4.0's kernel debugger on ARM (PINT).

NT 4.0 came with kernel debuggers for the x86, MIPS, Alpha and PowerPC; PINT
ports the kernel's side to ARM (kernel/kd, hal/jxport.c: the PL011 UART on
GPIO 14/15) but there is no host. This is one, enough to debug a driver:
the kernel's DbgPrint output, a report of every exception with registers and
the nearest symbols, memory, breakpoints, continue.

    pintkd.py PORT [options]
        PORT   a serial device (/dev/cu.usbmodem2102), tcp:HOST:PORT, or
               unix:PATH (QEMU: -serial unix:PATH,server=on,wait=off)
        -b BAUD        the BAUDRATE of OSLOADOPTIONS (default 115200; the
                       HAL's own default without BAUDRATE= is 19200)
        -m DIR|FILE    linker .map files, for names (several -m are fine)
        -l FILE        append everything shown to FILE
        -c SOCKET      also take commands on a unix socket (see --send)
        -g             never stop: report an exception and continue it
        -r FILE        write every byte received to FILE (the firmware and the
                       HAL also print text on this port before the debugger
                       takes it)
    pintkd.py --send SOCKET 'command'    run one command, print what it says

The machine boots with DEBUG in its OSLOADOPTIONS (kd\\kdinit.c: DEBUG,
BAUDRATE=n, CRASHDEBUG).

Commands (while the machine is stopped, except those marked *):
    g                continue          gn    continue, exception not handled
    r                registers
    d ADDR [N]       bytes             dd ADDR [N]   words
    ed ADDR VALUE..  write words
    ln ADDR          the nearest symbol
    k [N]            the stack: every word on it that is an address in code
    lm               loaded modules (PsLoadedModuleList)
    bp ADDR          set a breakpoint (prints its handle)   bc HANDLE   clear
    ver              the kernel's version record
    reboot           restart the machine
    break *          stop the machine (a break-in)
    wait [SECONDS] * let it run until it stops or the time passes
    status *         running or stopped
    q *              leave
ADDR is hex, or module!symbol, or symbol, with an optional +hex.

What PINT's kernel does not do yet (found with this program, 2026-11-11):
  - With more than one processor, a debugger event while another processor runs
    user code is followed by STOP 0x2B (PANIC_STACK_SWITCH) at that processor's
    next system call. Boot with NUMPROC=1 beside DEBUG, or use CRASHDEBUG (the
    debugger then wakes only at a bug check).
  - Its clock interrupt never polls for a break-in, so `break` does nothing:
    the machine stops at exceptions, bug checks and breakpoints only. Put
    EngDebugBreak() / DbgBreakPoint() where a driver should stop.

The protocol is NT 4.0's (public\\sdk\\inc\\windbgkd.h, ntos\\kd\\kdcomio.c):
a 16-byte header (leader, type, byte count, id, checksum), the data, 0xAA;
each data packet is acknowledged by a control packet with its id.
"""
import glob
import os
import re
import select
import socket
import struct
import sys
import time

LEADER, CTL_LEADER, TRAILER, BREAKIN = 0x30, 0x69, 0xAA, 0x62
T_STATE, T_MANIP, T_IO, T_ACK, T_RESEND, T_RESET = 1, 2, 3, 4, 5, 6
INITIAL_ID, SYNC_ID = 0x80800000, 0x800
EXC_STATE, LOAD_STATE = 0x3030, 0x3031
A_READ, A_WRITE, A_GETCTX, A_SETCTX, A_WRITEBP, A_RESTOREBP, A_CONTINUE = range(0x3130, 0x3137)
A_REBOOT, A_VERSION = 0x313B, 0x3146
A_PRINT, A_GETSTR = 0x3230, 0x3231
DBG_CONTINUE, DBG_NOT_HANDLED = 0x00010002, 0x80010001
MANIP_SIZE = 52  # sizeof(DBGKD_MANIPULATE_STATE): 12 + the largest member, GET_VERSION (40)
REGS = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
        "sp", "lr", "pc", "cpsr"]
EXC_NAMES = {0x80000003: "breakpoint", 0x80000004: "single step", 0xC0000005: "access violation",
             0xC000001D: "illegal instruction", 0xC0000094: "divide by zero",
             0x80000002: "datatype misalignment", 0xC00000FD: "stack overflow"}


class Port:
    def __init__(self, spec, baud):
        if spec.startswith("tcp:"):
            h, p = spec[4:].rsplit(":", 1)
            self.s = socket.create_connection((h, int(p)))
            self.fd, self.sock = self.s.fileno(), True
        elif spec.startswith("unix:"):
            self.s = socket.socket(socket.AF_UNIX)
            self.s.connect(spec[5:])
            self.fd, self.sock = self.s.fileno(), True
        else:
            import termios
            self.fd = os.open(spec, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(self.fd)
            a[0] = 0
            a[1] = 0
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            a[3] = 0
            a[4] = a[5] = getattr(termios, "B%d" % baud)
            a[6][termios.VMIN] = 0
            a[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
            self.sock = False
        self.rx = b""
        self.raw = None

    def fileno(self):
        return self.fd

    def fill(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        d = self.s.recv(4096) if self.sock else os.read(self.fd, 4096)
        if not d and self.sock:
            raise EOFError("the port closed")
        self.rx += d
        if self.raw and d:
            self.raw.write(d)
            self.raw.flush()
        return bool(d)

    def take(self, n, timeout):
        end = time.time() + timeout
        while len(self.rx) < n:
            left = end - time.time()
            if left <= 0:
                return None
            self.fill(left)
        d, self.rx = self.rx[:n], self.rx[n:]
        return d

    def write(self, d):
        while d:
            _, w, _ = select.select([], [self.fd], [], 5)
            if not w:
                raise IOError("the port does not take data")
            n = self.s.send(d) if self.sock else os.write(self.fd, d)
            d = d[n:]


class Maps:
    """Names from linker .map files: module -> [(rva, name)], read when asked for."""

    def __init__(self):
        self.files = {}      # module -> path
        self.mods = {}       # module -> [(rva, name)]

    def load(self, path):
        files = glob.glob(os.path.join(path, "*.map")) + glob.glob(os.path.join(path, "*.MAP")) \
            if os.path.isdir(path) else [path]
        for f in files:
            self.files[os.path.splitext(os.path.basename(f))[0].lower()] = f

    def has(self, mod):
        return mod.lower().rsplit(".", 1)[0] in self.files

    def syms(self, mod):
        mod = mod.lower().rsplit(".", 1)[0]
        if mod not in self.mods:
            base, syms = None, []
            if mod in self.files:
                for line in open(self.files[mod], errors="replace"):
                    m = re.search(r"Preferred load address is ([0-9A-Fa-f]{8})", line)
                    if m:
                        base = int(m.group(1), 16)
                        continue
                    m = re.match(r"\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{8})\s", line)
                    if m and base is not None:
                        a = int(m.group(2), 16)
                        if a >= base:
                            syms.append((a - base, m.group(1)))
                syms.sort()
            self.mods[mod] = syms
        return self.mods[mod]

    def near(self, mod, rva):
        syms = self.syms(mod)
        if not syms:
            return None
        lo, hi = 0, len(syms)
        while lo < hi:
            mid = (lo + hi) // 2
            if syms[mid][0] <= rva:
                lo = mid + 1
            else:
                hi = mid
        if not lo:
            return None
        r, n = syms[lo - 1]
        return "%s+0x%x" % (n, rva - r) if rva != r else n

    def find(self, name, mods):
        """(module, rva) of a symbol, looking in `mods` in that order."""
        for m in mods:
            for r, n in self.syms(m):
                if n == name or n.lstrip("_") == name:
                    return m, r
        return None


class Kd:
    def __init__(self, port, maps, log, never_stop):
        self.port, self.maps, self.never_stop = port, maps, never_stop
        self.log = open(log, "a") if log else None
        self.next_id = self.expect_id = INITIAL_ID
        self.halted = False
        self.state = None        # the last exception's header
        self.cpu = 0
        self.modules = {}        # name -> (base, size), from load notices and lm
        self.capture = None
        self.printed_nl = True

    # ---- output ----------------------------------------------------------------
    def say(self, text, end="\n"):
        text += end
        sys.stdout.write(text)
        sys.stdout.flush()
        if self.log:
            self.log.write(text)
            self.log.flush()
        if self.capture is not None:
            self.capture.append(text)

    # ---- packets ---------------------------------------------------------------
    def control(self, ptype, pid=0):
        self.port.write(struct.pack("<IHHII", CTL_LEADER * 0x01010101, ptype, 0, pid, 0))

    def read_packet(self, timeout):
        """(type, id, data), data None for a control packet; None when nothing came."""
        run, last = 0, None
        end = time.time() + timeout
        while run < 4:
            b = self.port.take(1, max(0.0, end - time.time()))
            if b is None:
                return None
            b = b[0]
            if b in (LEADER, CTL_LEADER):
                run = run + 1 if b == last else 1
                last = b
            else:
                run, last = 0, None
        h = self.port.take(12, 2)
        if h is None:
            return None
        ptype, count, pid, check = struct.unpack("<HHII", h)
        if last == CTL_LEADER:
            return ptype, pid, None
        if count > 4000:
            return None
        d = self.port.take(count + 1, 5)
        if d is None or d[-1] != TRAILER or sum(d[:-1]) & 0xFFFFFFFF != check:
            self.control(T_RESEND)
            return None
        d = d[:-1]
        if pid & SYNC_ID:            # the kernel started over
            pid &= ~SYNC_ID
            self.expect_id = pid
            self.next_id = INITIAL_ID
        self.control(T_ACK, pid)
        if pid != self.expect_id:    # one we have had already
            return None
        self.expect_id ^= 1
        return ptype, pid, d

    def send(self, ptype, data):
        pkt = struct.pack("<IHHII", LEADER * 0x01010101, ptype, len(data), self.next_id,
                          sum(data) & 0xFFFFFFFF) + data + bytes([TRAILER])
        for _ in range(8):
            self.port.write(pkt)
            end = time.time() + 3
            while time.time() < end:
                p = self.read_packet(end - time.time())
                if p is None:
                    continue
                if p[2] is None:
                    if p[0] == T_ACK and p[1] == self.next_id & ~SYNC_ID:
                        self.next_id ^= 1
                        return
                    if p[0] == T_RESEND:
                        break
                    if p[0] == T_RESET:
                        self.next_id = self.expect_id = INITIAL_ID
                        break
                else:
                    self.handle(p)
        raise IOError("the machine does not acknowledge")

    def manip(self, api, body=b"", data=b"", want=True):
        """A state-manipulate request; the reply's (status, body, data)."""
        hdr = struct.pack("<IHHI", api, 0, self.cpu, 0) + body
        hdr += bytes(MANIP_SIZE - len(hdr))
        self.send(T_MANIP, hdr + data)
        if not want:
            return None
        end = time.time() + 10
        while time.time() < end:
            p = self.read_packet(end - time.time())
            if p is None or p[2] is None:
                continue
            if p[0] == T_MANIP and len(p[2]) >= MANIP_SIZE:
                rapi, _, _, status = struct.unpack_from("<IHHI", p[2])
                if rapi == api:
                    return status, p[2][12:MANIP_SIZE], p[2][MANIP_SIZE:]
            else:
                self.handle(p)
        raise IOError("no answer to request %x" % api)

    # ---- what the kernel sends by itself -------------------------------------------
    def handle(self, p):
        ptype, _, d = p
        if ptype == T_IO:
            api, _, _, n = struct.unpack_from("<IHHI", d)
            if api == A_PRINT:
                text = d[16:16 + n].decode("latin-1").replace("\r\n", "\n")
                self.say(text, end="")
                self.printed_nl = text.endswith("\n")
            elif api == A_GETSTR:    # a prompt (an assertion): answer "i", ignore
                self.say(d[16:16 + n].decode("latin-1"), end="")
                rep = struct.pack("<IHHII", A_GETSTR, 0, 0, n, 1) + b"i"
                self.send(T_IO, rep)
        elif ptype == T_STATE:
            new, _, cpu, ncpu, thread, pc = struct.unpack_from("<IHHIII", d)
            if new == LOAD_STATE:
                plen, base, _, _, size, unload = struct.unpack_from("<IIIIIB", d, 20)
                name = d[-plen:].split(b"\0")[0].decode("latin-1") if plen else "?"
                short = name.replace("/", "\\").rsplit("\\", 1)[-1].lower()
                if not self.printed_nl:
                    self.say("")
                    self.printed_nl = True
                if unload:
                    self.modules.pop(short, None)
                    self.say("[unloaded %s]" % name)
                else:
                    self.modules[short] = (base, size)
                    self.say("[loaded %s at %08x]" % (name, base))
                self.cpu = cpu
                self.manip(A_CONTINUE, struct.pack("<I", DBG_CONTINUE), want=False)
            elif new == EXC_STATE:
                self.cpu = cpu
                self.state = d
                self.halted = True
                self.report(d, cpu, ncpu, thread, pc)
                if self.never_stop:
                    self.cont(True)

    def context_of(self, d, pc):
        for off in (128, 124):
            if len(d) >= off + 72:
                c = struct.unpack_from("<18I", d, off)
                if c[16] == pc or c[0] & 0x00200000:
                    return c[1:18]
        return None

    def report(self, d, cpu, ncpu, thread, pc):
        code, flags, _, addr, nparam = struct.unpack_from("<IIIII", d, 20)
        params = struct.unpack_from("<%dI" % min(nparam, 15), d, 40)
        first = struct.unpack_from("<I", d, 100)[0]
        if not self.printed_nl:
            self.say("")
            self.printed_nl = True
        self.say("\n*** %s (%08x) at %s, processor %d of %d, thread %08x, %s chance" % (
            EXC_NAMES.get(code, "exception"), code, self.name(pc), cpu, ncpu, thread,
            "first" if first else "second"))
        if params:
            self.say("    parameters: " + " ".join("%08x" % x for x in params))
        if code == 0xC0000005 and len(params) >= 2:
            self.say("    %s %08x" % ("writing" if params[0] else "reading", params[1]))
        c = self.context_of(d, pc)
        if c:
            self.show_regs(c)

    def show_regs(self, c):
        for i in range(0, 17, 4):
            self.say("    " + "  ".join("%-4s %08x" % (REGS[j], c[j]) for j in range(i, min(i + 4, 17))))
        self.say("    pc: %s\n    lr: %s" % (self.name(c[15]), self.name(c[14])))

    # ---- names -------------------------------------------------------------------
    def module_of(self, a):
        for n, (b, s) in self.modules.items():
            if b <= a < b + (s or 0x1000000):
                return n, b
        return None

    def name(self, a):
        m = self.module_of(a & ~1)
        if not m:
            return "%08x" % a
        n, b = m
        s = self.maps.near(n, (a & ~1) - b)
        return "%08x %s!%s" % (a, n, s if s else "+0x%x" % (a - b))

    def addr(self, text):
        """hex, a register, or [module!]symbol, each with an optional +hex."""
        base, _, off = text.partition("+")
        off = int(off, 16) if off else 0
        if base.lower() in REGS:
            r = self.regs()
            if not r:
                raise ValueError("no context")
            return r[REGS.index(base.lower())] + off
        if re.match(r"^[0-9A-Fa-f]+$", base):
            return int(base, 16) + off
        mod, _, sym = base.rpartition("!")
        if not self.modules:
            self.lm()
        loaded = {n.rsplit(".", 1)[0]: b for n, (b, _) in self.modules.items()}
        order = [mod.lower().rsplit(".", 1)[0]] if mod else \
            [m for m in ("ntoskrnl", "hal") if m in loaded] + sorted(set(loaded) - {"ntoskrnl", "hal"})
        f = self.maps.find(sym, [m for m in order if m in loaded])
        if not f:
            raise ValueError("no symbol %s in the loaded modules with maps" % text)
        return loaded[f[0]] + f[1] + off

    # ---- requests ----------------------------------------------------------------
    def read(self, a, n):
        out = b""
        while len(out) < n:
            want = min(n - len(out), 3000)
            st, body, data = self.manip(A_READ, struct.pack("<III", a + len(out), want, 0))
            got = struct.unpack_from("<I", body, 8)[0]
            out += data[:got]
            if st or got < want:
                break
        return out

    def write(self, a, data):
        st, body, _ = self.manip(A_WRITE, struct.pack("<III", a, len(data), 0), data)
        return st == 0

    def regs(self):
        st, _, data = self.manip(A_GETCTX, struct.pack("<I", 0x00200003))
        if st or len(data) < 72:
            return None
        return struct.unpack_from("<17I", data, 4)

    def cont(self, handled=True):
        self.manip(A_CONTINUE, struct.pack("<I", DBG_CONTINUE if handled else DBG_NOT_HANDLED),
                   want=False)
        self.halted = False

    def lm(self):
        st, body, _ = self.manip(A_VERSION)
        head = struct.unpack_from("<I", body, 12)[0]
        d = self.read(head, 4)
        if len(d) < 4:
            return
        e = struct.unpack("<I", d)[0]
        for _ in range(200):
            if e == head or not e:
                break
            ent = self.read(e, 0x34)
            if len(ent) < 0x34:
                break
            nxt, base, size = struct.unpack_from("<I", ent)[0], *struct.unpack_from("<I", ent, 0x18), \
                struct.unpack_from("<I", ent, 0x20)[0]
            ln, _, buf = struct.unpack_from("<HHI", ent, 0x2C)
            nm = self.read(buf, min(ln, 128)).decode("utf-16-le", "replace").lower() if buf else "?"
            self.modules[nm] = (base, size)
            e = nxt

    def command(self, line):
        w = line.split()
        if not w:
            return
        c, a = w[0].lower(), w[1:]
        if c == "status":
            self.say("stopped" if self.halted else "running")
        elif c == "break":
            self.port.write(bytes([BREAKIN]))
            self.say("break-in sent")
        elif c == "wait":
            self.run(float(a[0]) if a else 30)
            self.say("stopped" if self.halted else "still running")
        elif c == "q":
            raise KeyboardInterrupt
        elif not self.halted:
            self.say("the machine is running: break stops it")
        elif c in ("g", "gn"):
            self.cont(c == "g")
        elif c == "r":
            r = self.regs()
            self.show_regs(r) if r else self.say("no context")
        elif c in ("d", "dd"):
            base = self.addr(a[0])
            n = int(a[1], 0) if len(a) > 1 else 64
            data = self.read(base, n * (4 if c == "dd" else 1))
            if not data:
                self.say("%08x: not readable" % base)
            for i in range(0, len(data), 16):
                row = data[i:i + 16]
                if c == "dd":
                    txt = " ".join("%08x" % x for x in struct.unpack("<%dI" % (len(row) // 4), row[:len(row) // 4 * 4]))
                else:
                    txt = " ".join("%02x" % x for x in row).ljust(48) + " " + \
                        "".join(chr(x) if 32 <= x < 127 else "." for x in row)
                self.say("%08x  %s" % (base + i, txt))
        elif c == "ed":
            ok = self.write(self.addr(a[0]), b"".join(struct.pack("<I", int(x, 16)) for x in a[1:]))
            self.say("written" if ok else "not written")
        elif c == "ln":
            self.say(self.name(self.addr(a[0])))
        elif c == "lm":
            self.lm()
            for n, (b, s) in sorted(self.modules.items(), key=lambda x: x[1]):
                self.say("%08x %08x  %s%s" % (b, s, n, "" if self.maps.has(n) else "  (no map)"))
        elif c == "k":
            r = self.regs()
            if not r:
                self.say("no context")
                return
            if not self.modules:
                self.lm()
            self.say("pc %s\nlr %s" % (self.name(r[15]), self.name(r[14])))
            data = self.read(r[13], 4 * (int(a[0], 0) if a else 256))
            for i in range(0, len(data) - 3, 4):
                v = struct.unpack_from("<I", data, i)[0]
                if self.module_of(v & ~1):
                    self.say("%08x  %s" % (r[13] + i, self.name(v)))
        elif c == "bp":
            st, body, _ = self.manip(A_WRITEBP, struct.pack("<II", self.addr(a[0]), 0))
            self.say("breakpoint %d" % struct.unpack_from("<I", body, 4)[0] if not st else "not set (%08x)" % st)
        elif c == "bc":
            st, _, _ = self.manip(A_RESTOREBP, struct.pack("<I", int(a[0], 0)))
            self.say("cleared" if not st else "not cleared (%08x)" % st)
        elif c == "ver":
            st, body, _ = self.manip(A_VERSION)
            maj, mnr, proto, fl, kb, plm, mach = struct.unpack_from("<HHHHIIH", body)
            self.say("NT build %d (%s), protocol %d, %s, machine %04x, kernel at %08x, modules at %08x" % (
                mnr, "checked" if maj == 0xC else "free", proto, "MP" if fl & 1 else "UP", mach, kb, plm))
        elif c == "reboot":
            self.manip(A_REBOOT, want=False)
            self.halted = False
        else:
            self.say("commands: g gn r d dd ed ln k lm bp bc ver reboot break wait status q")

    # ---- the loop ----------------------------------------------------------------
    def run(self, seconds):
        """Let the machine run: show what it prints, until it stops."""
        end = time.time() + seconds
        while not self.halted and time.time() < end:
            p = self.read_packet(min(0.5, max(0.0, end - time.time())))
            if p and p[2] is not None:
                self.handle(p)

    def serve(self, ctl):
        srv = None
        if ctl:
            if os.path.exists(ctl):
                os.unlink(ctl)
            srv = socket.socket(socket.AF_UNIX)
            srv.bind(ctl)
            srv.listen(2)
        tty = sys.stdin.isatty()
        self.control(T_RESET)
        self.say("pintkd: listening%s" % ("; commands at the prompt" if tty else ""))
        while True:
            fds = [self.port.fd] + ([srv] if srv else []) + ([sys.stdin] if tty else [])
            if self.port.rx:
                r = [self.port.fd]
            else:
                r, _, _ = select.select(fds, [], [], 1)
            if self.port.fd in r and not self.halted:
                p = self.read_packet(0.2)
                if p and p[2] is not None:
                    self.handle(p)
            elif self.port.fd in r:
                self.port.fill(0)        # stopped: nothing is due; keep the buffer drained
                self.port.rx = b""
            if srv in r:
                c, _ = srv.accept()
                try:
                    line = c.makefile().readline().strip()
                    self.capture = []
                    try:
                        self.command(line)
                    except (ValueError, IndexError, IOError) as e:
                        self.say("? %s" % e)
                    c.sendall("".join(self.capture).encode("latin-1", "replace"))
                finally:
                    self.capture = None
                    c.close()
            if tty and sys.stdin in r:
                line = sys.stdin.readline()
                if not line:
                    return
                try:
                    self.command(line.strip())
                except (ValueError, IndexError, IOError) as e:
                    self.say("? %s" % e)


def main():
    a = sys.argv[1:]
    if a[:1] == ["--send"]:
        s = socket.socket(socket.AF_UNIX)
        s.connect(a[1])
        s.sendall(a[2].encode() + b"\n")
        out = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
        sys.stdout.write(out.decode("latin-1"))
        return
    if not a or a[0].startswith("-"):
        print(__doc__)
        return
    spec, baud, log, ctl, never, raw = a[0], 115200, None, None, False, None
    maps = Maps()
    i = 1
    while i < len(a):
        if a[i] == "-b":
            baud = int(a[i + 1]); i += 2
        elif a[i] == "-m":
            maps.load(a[i + 1]); i += 2
        elif a[i] == "-l":
            log = a[i + 1]; i += 2
        elif a[i] == "-c":
            ctl = a[i + 1]; i += 2
        elif a[i] == "-g":
            never = True; i += 1
        elif a[i] == "-r":
            raw = a[i + 1]; i += 2
        else:
            raise SystemExit("unknown option %s" % a[i])
    port = Port(spec, baud)
    if raw:
        port.raw = open(raw, "ab")
    kd = Kd(port, maps, log, never)
    try:
        kd.serve(ctl)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
