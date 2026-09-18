#!/usr/bin/env python3
"""Turn tools/pcsample.c output into where each thread spends its time.

    pcsample-report.py SAMPLES EXE [LIBDIR] [--callers FUNCTION]

SAMPLES is what pcsample printed on the IRIX machine. EXE is the exact binary
that was sampled -- the agent keeps its .symtab, and libvpx, mbedTLS and the
rest are linked in statically, so their functions are named too. LIBDIR holds
copies of the machine's own shared libraries (libc.so.1, libX11.so.1,
libpthread.so ...): IRIX DSOs are quickstarted at fixed addresses, so an
address inside one is resolved against the copy's own symbols. Fetch them from
the machine being sampled, not from the sysroot -- they can differ.

For each thread: how many samples, its CPU time, and a histogram by function.
A sample whose thread was asleep is counted as "(asleep)". --callers F then
lists the return addresses of the samples that landed in F, which for a leaf
function like memcpy names who called it.

This is how the O2's "spin" was found on 2026-09-18: a thread with thousands of
milliseconds of CPU and every sample in a two-instruction loop in libX11's
flush path (see src/input_shim.c, g_lock).
"""
import bisect
import collections
import os
import subprocess
import sys

NM = os.environ.get("LLVM_NM", "llvm-nm")
for cand in ("/opt/cross/bin/llvm-nm", "/usr/lib/llvm-18/bin/llvm-nm"):
    if NM == "llvm-nm" and os.path.exists(cand):
        NM = cand


def symtab(path):
    """Sorted addresses and a map address -> name, from .symtab and .dynsym.

    IRIX's own DSOs report their dynamic symbols with type '?', so every type
    that can be code is taken."""
    names = {}
    for flag in ([], ["-D"]):
        out = subprocess.run([NM, "-n", *flag, path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) >= 3 and p[1] in "TtWw?":
                names[int(p[0], 16)] = p[2]
    return sorted(names), names


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    callers = None
    if "--callers" in sys.argv:
        callers = sys.argv[sys.argv.index("--callers") + 1]
        args = [a for a in args if a != callers]
    if len(args) < 2:
        sys.exit(__doc__)
    samples, exe = args[0], args[1]
    libdir = args[2] if len(args) > 2 else None

    tables = [(0, 1 << 32, None, symtab(exe))]
    if libdir:
        for f in sorted(os.listdir(libdir)):
            addrs, names = symtab(os.path.join(libdir, f))
            if addrs:
                tables.insert(0, (addrs[0] & ~0xFFFF, addrs[-1] + 0x10000, f, (addrs, names)))

    def name(pc):
        for lo, hi, tag, (addrs, names) in tables:
            if lo <= pc < hi and addrs:
                i = bisect.bisect_right(addrs, pc) - 1
                if i >= 0 and pc - addrs[i] < 0x40000:
                    return names[addrs[i]] + (f" [{tag}]" if tag else "")
        return f"?{pc:#x}"

    per_thread = collections.defaultdict(collections.Counter)
    cpu = {}
    who_called = collections.Counter()
    for line in open(samples):
        p = line.split()
        if not p or p[0] != "T":
            continue
        tid, flags, epc, ra = int(p[2]), int(p[3], 16), int(p[4], 16), int(p[5], 16)
        cpu[tid] = int(p[7]) + int(p[8])
        asleep = flags & 0x8  # PR_ASLEEP
        where = "(asleep)" if asleep else name(epc)
        per_thread[tid][where] += 1
        if callers and not asleep and where.startswith(callers):
            who_called[f"{name(ra)} (ra {ra:#x})"] += 1

    for tid, c in sorted(per_thread.items()):
        total = sum(c.values())
        print(f"thread {tid}: {total} samples, cpu {cpu[tid]} ms")
        for fn, n in c.most_common(25):
            print(f"  {100 * n / total:5.1f}%  {fn}")
    if callers:
        print(f"callers of {callers}:")
        for k, n in who_called.most_common(15):
            print(f"  {n:5d}  {k}")


if __name__ == "__main__":
    main()
