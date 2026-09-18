Resume prompt: the O2, after it was made fast

Continue the IRIX agent -- R-DeskVint, binary `r-deskvint-irix`, inst product
`r_deskvint_irix`. `RESUME.md` §B THE O2, MADE FAST is the record of this work
(2026-09-18, second half) and §A HOSTED BUILD PIPELINE the pipeline. This file
is what is still open.

Dani's words, which set the priorities: the agent felt slow on the O2 -- the
mouse lagged, video updates were slow -- and "create versions for mips3 and
mips4".

## What was found, in one paragraph each

**The "boot-start spin" was two sessions at once.** The previous handover
blamed the boot path; the agent's own log disproves it (RESUME §B.1). Every
session queried the pointer through ONE unlocked X connection in
`src/input_shim.c`, and IRIX's `_XFlushInt` spins for ever when another
thread is mid-write and there is no lock. Two Macs connected at once froze a
session and left its thread spinning; each overlap added one. Fixed with a
mutex, a per-thread X I/O guard, and a reconnecting input connection.
`probes/xrace.c` wedged in under a second before and runs clean after.
`tools/pcsample.c` + `tools/pcsample-report.py` are how it was found, and how
anything like it should be found next time: CPU with no syscalls means sample
the PCs.

**The floor under every frame was copying.** A pointer move cost 190 ms of
encode at 1280x1024 (55 at 640x512); `patches/libvpx-vp8-copy-only-what-changed.patch`
makes it 46 (16), bitstream byte-identical (`encfloor verify`). libvpx's own
key frames every 128 frames (2.3 s each at full size) are off on IRIX.

**MIPS IV is built beside MIPS III** and one `.tardist` carries both; inst
picks by CPUARCH. Worth 3-6% on the O2.

## Where things stand

* Commits on `vintage-agents`, NOT PUSHED (CI runs on push; push only when
  Dani asks): 006513f18 spin fix, 58f159bd9 libvpx, 220146a52 key frames,
  c01a921af MIPS IV + packaging, 5177b49f6 version/inst version/logger/
  LoopClock, then the docs.
* The toolchain cache key changes with these (toolchain.sh, patches/,
  ports/toolchain/), so the first CI run rebuilds the toolchain, MIPS IV
  libraries included.
* `/opt/sgug-staging/usr/sgug/lib32/libvpx.a` was replaced by the new one
  (the old is beside it, `.before-copy-only-what-changed`). /opt has no MIPS
  IV tree: local builds without IRIX_TOOLCHAIN are MIPS III only, and say so.
* The O2 (`sgio2`, 192.168.99.41): see "On the O2" below for what is installed.

## Priorities, in order

1. DONE: two Macs at once on the O2, 2026-09-18 -- both sessions kept
   running, and idle together the agent used 230 ms of CPU per 10 s (RESUME
   §B, "Verified on the O2"). If a session ever freezes again, pcsample the
   agent first: a thread with CPU and no syscalls is the signature.
2. **Measure two things on the O2 that could not be measured without it.**
   - `patches/libvpx-vp8-key-frame-bpred-once.patch`: a key frame no longer
     codes its B_PRED macroblocks twice. Bitstream-identical; the time is
     unknown (the emulator was too noisy). `encfloor 1280 1024 1` against
     `/tmp/encfloor-new` on the O2, which predates it. A key frame was 2.3 s
     at 1280x1024, 0.6 s at 1/2 -- every session start, refresh and scale
     change pays it. What is left is the 4x4 intra search itself (10 modes x
     16 subblocks) and the transforms; narrowing the search for screen content
     would change the bitstream and needs a visual check.
   - `probes/convbench.c`: the capture shim's ABGR->I420 conversion against a
     word-load variant and a MIPS IV prefetch variant, all checked
     byte-identical (in an R5000 guest). The shipped kernel runs at ~90 ns a
     source pixel on the O2 -- memory-bound -- and only the O2 can say whether
     either variant helps; build it both ways (compile and link separately for
     MIPS IV, or the wrapper does not raise the ISA) and move the winner into
     `rd_abgr_to_i420_rect`.
   Also worth knowing: with two viewers connected, every change is captured,
   converted and encoded once per viewer -- a forgotten second window halves
   the speed. Sharing one encode between sessions at the same scale is the fix
   and a real change to how sessions are built.
3. **Console heartbeats** cost ~4% of the O2 at idle, measured with pcsample:
   ~3/4 the TLS handshake (P-384 ECDHE, signature verify), ~1/5 parsing the
   121 CA certificates. Parsing once saves the fifth; reusing the connection
   (the request says `Connection: close` today) saves the rest.
4. `screen_content` 2 vs 0 was not re-decided: within a few percent on
   synthetic content, 2.4x the bytes on the O2's real desktop (whole-frame
   probe). Measure on real content in a session before changing it.

## On the O2

`r_deskvint_irix 2026091865`: a build of this work from before the version
display and the logger change (it still says `R-DeskVint 0.1.0`), MIPS IV
agent, installed with inst on 2026-09-18 with Dani's go-ahead. The boot start
is on (`chkconfig r_deskvint_irix on`, init script and rc links in place).
The previous agent binary is kept as `/usr/local/sbin/r-deskvint-irix.cf5c43c05`.

The next thing to install is `dist/r-deskvint-irix-20260918-85a3efe43-n32.tardist`
(inst version 2026091885; built from the key-frame commit, gendist in an R5000
guest). The packaging is the same as 5177b49f6's, which passed the install
test in both an R5000 and an R4400 guest. Ask first: it restarts the agent. The
install is what `/tmp/o2-install.sh` on the O2 does: stop the agent, `inst -f`
with `install standard / go / quit`, start it with the helper.

## Reaching the O2

`rexec` on port 512, `root`, empty password; there is no rexec client on the
build host. From Python:

```python
import socket
def o2(cmd, stdin=None, host='192.168.99.41'):
    s = socket.create_connection((host, 512), timeout=300)
    s.sendall(b'0\0root\0\0' + cmd.encode() + b'\0')
    assert s.recv(1) == b'\0'
    if stdin is not None:           # e.g. o2('cat > /tmp/f', open(p,'rb').read())
        s.sendall(stdin)
    s.shutdown(socket.SHUT_WR)
    out = b''
    while (d := s.recv(65536)): out += d
    return out.decode(errors='replace')
```

IRIX tools there: `grep` has no `\|` (use `egrep`), `par -s -SS -t N -p PID`
traces syscalls, rld writes its errors to `/var/adm/SYSLOG`, `ps -e` truncates
names to eight characters (`ps -e -o pid,args`), `sum` is SysV (use `sum -r`
to compare with a Linux `sum -r`). `/tmp/pcsample` is there; copy the
machine's own `libc.so.1`, `libX11.so.1` and `libpthread.so` for
`pcsample-report.py` -- they are not the sysroot's.

## Boundaries

* Do not modify `~/repos/iris`. Do not change the O2's xdm configuration
  (`grabServer` is False there on purpose -- and it is why the greeter serves
  clients, which the old handover had backwards).
* Rebooting the O2, stopping its agent, or installing on it interrupts Dani's
  sessions -- say so first.
* Never commit, cache or upload licensed material (the image, the sysroot).
* Changes to `rustdesk-ppc-agent/src` are shared with the Mac and SPARC agents:
  gate IRIX behaviour with `cfg(target_os = "irix")` and run the host suite
  (`cargo test` in `rustdesk-ppc-agent`, with a scratch `CARGO_TARGET_DIR`).
* Push only when asked; CI runs on push.
