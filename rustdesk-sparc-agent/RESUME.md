# RESUME — RustDesk agent for Solaris 10 / SPARC

State of the port, the environment it runs in, what is already proved and what
is not, and the traps that cost time the first time. This is the source of
truth; `RESUME-PROMPT.md` is the short prompt that points here.

Last updated at the end of the session that got the agent serving a peer.

---

## 1. The machine

**Sun Blade 2500**, `192.168.99.176`, user `dani` (uid 100, group `staff`), ssh
keys already work. Solaris 10 8/11 (`s10s_u10wos_17b`), one UltraSPARC IIIi at
**1600 MHz**, 4 GB RAM, ~15 GB free on `/`. XVR-600 framebuffer at
`/dev/fbs/jfb0`.

**No root.** `sudo` exists at `/opt/csw/bin/sudo` but wants a password;
`pfexec` is present without a profile. Ask for anything that needs it.

### Toolchain there

| | |
|---|---|
| C compiler | **`/opt/csw/bin/gcc-5.5`** (5.5.0) — required |
| also present | gcc 4.9.2 (`/opt/csw/bin/gcc`), g++ 4.9.2, gcc 3.4.3 (`/usr/sfw/bin/gcc`) |
| linker / ar | `/usr/ccs/bin/ld` (Solaris 5.10), `/usr/ccs/bin/ar`, GNU `ar` as `/opt/csw/bin/gar` |
| GNU userland | under **`/opt/csw/gnu`** (plain names: `diff`, …) |
| build tools | `gmake`, `gtar`, `rsync`, `curl`, `screen`, `python` (2.x), `bash` **3.00** at `/usr/bin/bash` |

gcc **4.9 is not enough**: it has no `__builtin_add_overflow`/`sub`/`mul`,
which mrustc emits for Rust's checked arithmetic. They arrived in gcc 5, and
4.9 compiles them as implicit declarations — the failure is three undefined
symbols at link time and nothing before it.

`cc1` is a 32-bit SPARC32PLUS binary, so it is capped near 4 GB of address
space however much RAM the machine has. Not a problem so far (libstd's 31 MB
unit goes through); `SPARC_BIG_TU_BYTES` in the ssh wrapper is the lever.

### X on the machine

The console is **not usable** and needs root to become so: `/dev/fbs/jfb0` is
`crw------- root root`, `Xsun` is not setuid, and while CDE sits at its greeter
`dtgreet` holds a server grab that hangs every client *inside `XOpenDisplay`*
(confirmed with `pstack`: blocked in `_XWaitForReadable` during the connection
handshake — not an auth failure, no error, just silence).

So the agent gets its own session: **`scripts/rd-session.sh start`** brings up
`Xvfb :2` + `dtwm` + `dtterm` — a real CDE desktop, startable by an ordinary
user, surviving logouts. `:0` is the console, `:1` is left for ad-hoc probes.
Use `/usr/X11/bin/Xvfb`; Sun's own `/usr/openwin/bin/Xvfb` has no `-screen`.

`Xsun` itself advertises DAMAGE, XFIXES, MIT-SHM, XTEST, RENDER, SHAPE, RECORD,
SYNC, XKEYBOARD, DOUBLE-BUFFER, SUN_OVL, XC-MISC (read out of the binary with
`strings`). **X client libraries live in two prefixes**: Xlib/Xext/Xtst under
`/usr/openwin/lib`, but **Xfixes and Xdamage under `/usr/openwin/sfw/lib`** —
both with `sparcv9/` builds, all sharing `/usr/openwin/include`. Looking only in
the first says the machine has no XFIXES, which is wrong.

### C libraries built for sparcv9

All in **`~/sparc-deps`** on the Blade, static, `-m64 -O2 -fPIC`, built by
`scripts/build-deps.sh` (tarballs staged in `/tmp` there):

libsodium 1.0.18 · zstd 1.5.6 · mbedTLS 3.6.2 · libvpx 1.13.1

Sources are fetched on the build host and copied over: Solaris 10's curl
carries a certificate store from 2011.

---

## 2. The build host

`/home/dani/repos/mrustc` — branch **`sparc-solaris-10`**, four commits:

```
24e0ed0a  hir: Support `#[repr(align(N))]` on unions
400e373a  trans: Add sparcv9-sun-solaris
3587bd55  minicargo: Allow STD_ENV_ARCH to be overridden
109ddad1  expand: Fix `{:02x?}` leaving "?}" in the formatted output
```

`/home/dani/repos/rustdesk` — the agent in `rustdesk-sparc-agent/`, plus
Solaris arms added to the shared PowerPC sources (`session.rs`, `main.rs`,
`input.rs`, `cursor.rs`, `sys.rs`).

### From a fresh clone

Both repos carry everything that is *ours*; three things are fetched rather
than stored, and this is the order:

```sh
git clone git@github.com:danifunker/mrustc.git   -b sparc-solaris-10
git clone git@github.com:danifunker/rustdesk.git -b vintage-agents
cd mrustc && make -j8 && make -f minicargo.mk    # downloads rustc-1.74.0-src itself

# The crate sources. Gitignored at 59 MB; Cargo.lock is tracked, so this
# reproduces exactly the versions that built the current binaries.
cd ../rustdesk/rustdesk-ppc-agent
cargo vendor vendor && ./patches/protobuf-ub.py

# The C libraries, on the Blade. fetch-deps.sh has the URLs and the SHA-256 of
# the exact tarballs that built the current ~/sparc-deps.
cd ../rustdesk-sparc-agent
./scripts/fetch-deps.sh --push dani@192.168.99.176
ssh dani@192.168.99.176 'sh /tmp/build-deps.sh all'
```

What a clone cannot carry is the Blade: Solaris 10 8/11, OpenCSW's gcc 5.5
(`pkgutil -i gcc5core`), and the X libraries at the two prefixes in §1. Those
are machine state, and §1 is the record of them.

### Building the standard library (once)

```sh
cd ~/repos/mrustc
MRUSTC_TARGET_VER=1.74 SPARC_HOST=192.168.99.176 SPARC_CC=/opt/csw/bin/gcc-5.5 \
CC_sparcv9_sun_solaris=~/repos/rustdesk/rustdesk-sparc-agent/scripts/sparc-cc-remote.py \
  make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 \
       MRUSTC_TARGET=sparcv9-sun-solaris OVERRIDE_SUFFIX=-solaris \
       STD_ENV_ARCH=sparc64 PARLEVEL=2
```

Lands in `output-1.74.0-sparcv9-sun-solaris`. About 9 minutes of Blade CPU for
the C half (libcore 222 s, libstd 123 s) plus the front end here.
`scripts/recompile-objects.sh` replays just the codegen when only the C
compiler changes.

**The union `repr(align)` change altered rlib metadata.** The host tree was
rebuilt for it; `output-1.74.0-powerpc-apple-darwin*` and `output-1.74.0-g[345]`
were **not**, and will fail with `Bad tag for HIR::TypeItem` if used with the
current mrustc. Rebuild them before the next PowerPC build.

### Building the agent

```sh
cd ~/repos/rustdesk/rustdesk-sparc-agent
SPARC_HOST=192.168.99.176 ./scripts/build-sparc.sh
SPARC_HOST=192.168.99.176 ./scripts/build-sparc.sh run captest --frames 5
```

Rust is compiled here, the C it becomes is compiled on the Blade over ssh by
`scripts/sparc-cc-remote.py`, and the binary comes back. No cross toolchain is
involved. A build cycle is roughly 8–12 minutes.

---

## 3. What is proved, and how

Every number below was measured on the Blade, not inferred.

**Rust itself** (`probes/smoke.rs`, built with `-C panic=unwind`):

```
os=solaris arch=sparc64 pointer_width=64
native bytes of 0x01020304 = [01, 02, 03, 04] (big-endian)
u128 align=16 big/7=16203922234330403022065457496750867212
atomics: u8=232 (expect 232 after wrap) u64=1000 mutex=1000
catch_unwind: unwound OK
slept 120.063999ms; bound 127.0.0.1:33385; file round-trip ok
```

mrustc defaults to `panic=abort`; without `-C panic=unwind` a panic kills the
process instead of unwinding.

**The portable half** (`prototest`, one binary, all of it checked against bytes
rather than against itself):

```
RegisterPeer wire bytes: 0a 0b 73 70 61 72 63 2d 62 6c 61 64 65 10 84 86 88 08
PNG: 75 bytes, IHDR says 4x2      (carried back here: chunk CRCs valid, pixels right)
white 16x16 -> Y=235 U=128 V=128
secretbox: sealed 38 bytes, opened them again
ed25519: signed IdPk verifies and parses, 111 bytes, pk 32 bytes
login hash: 32 bytes, stable, and discriminating
vp8: keyframe 1221 bytes, first bytes 10 3b 00 9d 01 2a
cpu: "SUNW,Sun-Blade-2500 @ 1.6 GHz"  memory: "4 GB"  os: "Solaris 10"
```

**Capture** (`captest`, against `Xvfb :2` at 1280×1024 with `xclock -update 1`):

| | DAMAGE + MIT-SHM | MIT-SHM + comparison |
|---|---|---|
| full screen read | 4.9 ms | 4.9 ms |
| 20 idle polls | **1.4 ms total** | 168 ms total |
| a change reports | `181x181 at 51,51` | a band, `1280x64 at 0,96` |

Pixel order is **A,R,G,B** (the PowerPC Mac's, *not* the IRIX port's A,B,G,R),
stride is exactly `width*4`, `XGetImage` costs 200 ms against SHM's 4.9.

**Input** (`probes/inputtest.c`): pointer moves land exactly, buttons work,
typing reaches the focused window; typing into the terminal produces damage
rectangles of `6x13 at 93,90` — one character cell.

**Cursor** (`probes/cursortest.c`): a 9×16 I-beam with hotspot 4,8 over dtterm,
drawn as text so a decoding error is visible rather than merely suspected.

**Clipboard** (`probes/cliptest.c`, two processes): 21 bytes transferred over a
real SelectionRequest, and exactly one change reported when ownership moves.

**The fused downscale-and-convert** (`convtest`, no display needed, and the
`to_i420` line of `captest`, on a real desktop). `rd_argb_to_i420_rect` is the
one piece here adapted from the IRIX port's C rather than ported from the
shared Rust, and the way it would be wrong is a red/blue swap. So it is checked
two ways.

Against synthetic images, on the machine:

```
factor 1 vs convert::argb_to_i420 (rc 0): Y max 0 (0 of 2048), U max 1, V max 1
red   rgb(255,0,0)   -> Y= 82 U= 90 V=240   ok        <- a swap reports blue's
blue  rgb(0,0,255)   -> Y= 41 U=240 V=110   ok           chroma here, and vice
green rgb(0,255,0)   -> Y=144 U= 54 V= 34   ok           versa. 150 codes apart.
factor 1/2/4/8 on constant boxes: Y max 0 U max 0 V max 0  ok
1/2 of a checkerboard: Y=126 (not 235 or 16)  ok       <- the box really averages
rect 8,8..24,24: 0 inside unwritten, 0 outside overwritten, 0 disagree  ok
odd bounds 5,5..11,11 snap to 4,4..12,12: 0 holes, 0 spill  ok
factor 3, 0, -1, 6, 16 and an oversized destination -> all refused
```

And against a real 1280x1024 desktop, through the whole `Capturer` — its
stride, its buffer length, its pixel-order gate, none of which a synthetic test
reaches:

| | |
|---|---|
| `to_i420` at 1/1 | **150 ms**, against the shared Rust converter's **1868 ms** |
| at 1/2 (640x512) | **21 ms** |
| at 1/4 (320x256) | **9.3 ms** |
| vs `convert::argb_to_i420` | Y **bit-identical**, U and V differ by at most 1 |

The luma is bit-identical by construction. The chroma cannot be: the shared
converter averages each channel and then weights it, this one weights the sums
and shifts once — which keeps the fractional part of the average through the
weighting, and is a difference of at most one code. Measured, not assumed.

The 12x gap against the shared converter is not a fair fight and is not meant
to be: that one is mrustc-generated C with Rust's bounds checks still in the
inner loop, and this one is hand C at `-O2`. It is the reason the fused path
exists.

---

## 4. THE AGENT RUNS

`rustdesk-agent` is built from the PowerPC tree's own `main.rs` and serves a
peer on this machine. `src/bin/testpeer.rs` — taken from the IRIX port's tree,
so it is the same client that port was proved with — speaks the real protocol:
signed identity, sealed key, password hash, login, then it counts and decodes
the video that comes back.

```
agent id  : 8vx1okqkt
display   : 1280x1024
[  0.056] INFO  agent listening on 0.0.0.0:21118 (id 8vx1okqkt)
[  0.057] INFO  lan discovery listening on udp/21119
[  6.035] INFO  peer 'testpeer' logged in -- entering message loop
[  6.039] INFO  encoder: 640x512, 375 kbps, 1 thread(s) of 1 processor(s)
[  6.063] INFO  video: 1280x1024 framebuffer served at 640x512 (1/2)
[  6.064] INFO  capture path: DAMAGE + MIT-SHM

  video frames   32 (1 key), 130186 bytes total
  first frame    207 ms after login
  decoded        17 ok, 0 refused
VERDICT: the agent is serving video to a peer.
```

So TCP, framing, the direct-IP handshake, the password hash, `PeerInfo`, the
video pump, VP8 encoding, delivery and **decoding at the peer** all work.

### 4a. And the picture is the right colour

The one thing this port's fused converter could plausibly get wrong is a
red/blue swap, so it is checked at four levels rather than one, and the last is
end to end:

1. `convtest`, synthetic, on the machine — red is `Y=82 U=90 V=240`, blue is
   `Y=41 U=240 V=110`, exactly.
2. `captest`, a real frame, against `convert::argb_to_i420` — luma identical.
3. A red `dtterm` and a blue one, read straight out of the framebuffer:
   `rgb(255, 0, 0)` and `rgb(0, 19, 255)`.
4. **The same two windows through the whole pipeline** — capture, downscale,
   I420, VP8, TCP, decode at the peer — arriving as `rgb(241, 4, 12)` and
   `rgb(9, 19, 242)`.

An aggregate check on an ordinary desktop agrees: over the coloured pixels of a
real CDE screen, the picture as sent fits the framebuffer with a mean error of
16.8 per channel, and the same picture with red and blue exchanged fits at
64.8. A factor of 3.9 in the right direction.

## 5. Measured on this machine

**Frame rate**, against a `dtterm` running `while true; do ls -l /usr/bin; done`
— a screen that never stops changing, which is the ceiling rather than the
common case. 25 s per row, `testpeer` not decoding (decoding on the same
single-processor machine roughly halves it, and would be a number about the
decoder):

| the peer asks for | served | fps | bytes/frame |
|---|---|---|---|
| Low | 320x256 (1/4) | **12.8** | 1146 |
| Balanced — the default | 640x512 (1/2) | **8.5** | 5150 |
| Best | 1280x1024 (1/1) | **3.8** | 30564 |

**Where a frame goes**, mean over every frame of those runs, in ms:

| served | probe | read | conv | enc | send | total | bytes |
|---|---|---|---|---|---|---|---|
| 320x256 | 13.3 | 0.1 | 2.7 | 8.7 | 0.4 | **27.2** | 1081 |
| 640x512 | 18.0 | 0.0 | 10.7 | 42.3 | 2.1 | **76.2** | 4886 |
| 1280x1024 | 25.9 | 0.0 | 14.6 | 171.1 | 7.0 | **220.5** | 30139 |

Three things fall out of that table, and they settle questions this file used
to list as open:

* **`DEFAULT_SCALE = 2` is right**, and is no longer a guess inherited from an
  emulated Indy. 8.5 fps at 640x512 is a usable desktop; 1/1 is still usable at
  3.8 fps for a peer that asks for it, and 1/4 buys 12.8.
* **The encoder is the cost**, by a wide margin at every size — 171 of 220 ms at
  1/1, 42 of 76 at 1/2. That is what makes the resolution dial the right dial.
* **`read` is 0.0 ms** everywhere. The DAMAGE path means a band is never
  re-read: the pixels are already in the canvas from the poll. And **`conv` is
  2.7-14.6 ms**, against the 1868 ms the shared Rust converter takes for one
  full-size frame. The fused path is doing exactly what it was written for.

An idle screen costs nothing: with only `xclock -update 1` on the display, all
three scales report 2.0 fps, because a 1 Hz clock is two damage events a second
and the agent sends what changed and nothing else.

### 5a. The encoder `Tune` — one row of it is probably wrong

`rustdesk-agent --probe-display` sweeps the encoder at 1280x1024, median of
three, in ms for a still frame and a small change:

| config | still | small | bytes |
|---|---|---|---|
| baseline (profile 0, screen_content 0) | 308 | 399 | 14790 |
| **profile 3 (no loop filter, full pel)** | **254** | **291** | 14885 |
| profile 2 (no loop filter) | 255 | 382 | 14885 |
| static_threshold 30000 | 251 | 265 | 14790 |
| screen_content 1 | 367 | 368 | **28404** |
| **screen_content 2** | 376 | 318 | **28404** |

`video_tune()` sets `profile: 3, screen_content: 2` for this target, inherited
from IRIX. **Profile 3 is confirmed** — it is the fastest row on both columns.
**`screen_content: 2` is the suspect**: on its own it is slower than baseline
*and* it nearly doubles the bytes, 28404 against 14790, for the same picture.

Two cautions before acting on that. The sweep varies one knob at a time from
the *baseline* Tune, so it never measures `profile 3 + screen_content 2` — the
combination actually in use. And its "`<- in use`" marker points at the
baseline row, because the sweep does not know about `video_tune()`; that marker
is wrong on this platform. So the next move is to measure the combination, not
to delete the knob.

`static_threshold 30000` is the other row worth a look: 251/265 against the
current 1000's 308/399.

## 6. After that, in order

1. **The `screen_content` question above**, which is a bandwidth question and
   the cheapest win on the table.
2. **A real RustDesk client**, not `testpeer`. Everything above is the protocol
   proved against an implementation of the same protocol; a stock client is the
   thing that finds where they differ. Direct-IP to `192.168.99.176:21118`,
   password `sparctest` as left by the last run. `ipfilter` is disabled on the
   Blade, so nothing is in the way.
3. **The Motif settings panel**, following `rustdesk-irix65-agent/gui/`: a
   *second binary* with no behaviour of its own, each button running one verb
   of a helper script that runs one `rustdesk-agent --flag`. Solaris has Motif
   **2.1.0** with 64-bit libraries at `/usr/dt/lib/sparcv9/libXm.so` and
   headers in `/usr/dt/include/Xm`; the IRIX panel is Motif 1.2 code, which 2.1
   accepts.
4. **Packaging.** Nothing exists. The IRIX port's `inst/` and release script
   are the model; Solaris wants a `pkgadd` datastream.


## 7. Loose ends

* **No scroll wheel.** The pointer has three buttons, so X buttons 4 and 5
  cannot be injected — `XTestFakeButtonEvent(5)` is a `BadValue`, not a no-op.
  The shim reads the button count at startup, declines, and says so once. If
  the console session (a real mouse) has five, this resolves itself there.
* **The console (`:0`).** Re-run `probes/xprobe.c` and `captest` against it
  when it becomes usable: a real framebuffer read will not cost what Xvfb's
  does, and the whole capture design rests on that number.
* **INCR clipboard transfers** (over 256 KB) are reported as empty rather than
  as a truncated fragment. Implementing INCR is the fix if it matters.
* **`rd_clip_changed` returns 0 rather than -1 when it cannot reach the
  display**, so `clipboard::changed()` answers `Some(false)` where the shared
  contract says `None`. The behaviour is right either way -- "no change" is the
  safe answer for an unreachable clipboard, and `available()` is checked
  separately -- but the contract in `clipboard.rs` says otherwise. One line in
  `clipboard_shim.c`; not fixed here because touching a `.c` drops the
  build-script cache and costs a full rebuild cycle.
* ~~**`SO_RCVTIMEO` on Solaris is assumed, not measured.**~~ Now measured: it is
  not there. See §8 — this is the assumption that cost the first end-to-end
  run, and it was written down as an assumption before it cost anything, which
  is the only reason it took ten minutes to find rather than an afternoon.
* **The PowerPC output trees are stale** (see §2) and will fail loudly.
* **`--probe-display` measures a path the agent no longer takes.** Its
  `argb->i420` line and its `total ... for a full-screen change` (2158 ms) are
  the unfused `convert::argb_to_i420` over the whole screen; the agent uses
  `Capturer::to_i420_rect` over the damage, which §5 measures at 2.7-14.6 ms.
  Its "C shim vs rust reference" labels are also wrong here — on Solaris both
  sides of that comparison are the Rust one, since `argb_to_i420_rows` only
  calls the shim on macOS. Harmless, but do not quote those numbers.
* **`probe shape` in `--probe-display` reports zeros** for every row. That is
  correct and deliberate: `probe_bytes` returns 0 on the DAMAGE path because an
  idle poll reads no pixels, and the sweep's knobs describe a sampled-checksum
  probe this platform does not have. See the doc comment on
  `Capturer::dirty_bands_tuned`.
* **The agent's Rust half is compiled at `-O0`,** and this was not a decision.
  Once `session`/`api`/`rendezvous`/`clipboard` joined the crate, mrustc's
  output for the library became a **82 MB** translation unit — ten times
  `SPARC_BIG_TU_BYTES` (8 MiB) — so `sparc-cc-remote.py` swapped mrustc's `-O1`
  for `-O0 --param ggc-min-expand=10 --param ggc-min-heapsize=32768`. Confirmed
  with `pargs` on the running `cc1`, not inferred.

  It matters less than it sounds: the hot loops are all in shims compiled
  separately at `-O2` (capture, the fused conversion, vpx, tls), which is why
  `to_i420` measures 150 ms rather than something dreadful. What is at `-O0` is
  the session loop, the protocol and the crypto glue.

  Whether it can be raised is **measurable and unmeasured**: that `cc1` peaked
  at ~244 MB RSS with 2.8 GB free, and it is a 32-bit SPARC32PLUS binary capped
  near 4 GB. `-O1` on the same unit could be anywhere inside or outside that.
  Try `SPARC_BIG_TU_BYTES=100000000` and watch the RSS before believing either
  answer. The PowerPC wrapper's answer to the same problem was to split the
  unit; the header of `sparc-cc-remote.py` says to add that here when a real
  failure asks for it.

---

## 8. Traps, all of which cost time once

**Solaris tooling**

* `/usr/bin/grep` has no `-q`, no `-r`, and no `\|` alternation. Use `egrep`,
  and `find … -exec grep`. A pattern with `\|` matches *nothing* and reports
  success, which is how a running `Xsun` was declared absent.
* There is no `timeout(1)` and no `head -c`. `tail -r` does exist.
* `pkill -f xprobe` kills the ssh command running it, because that command line
  contains the pattern. Match by process name.
* An ssh command runs a **non-login shell** whose PATH has neither
  `/opt/csw/bin` nor rsync. The wrapper sets `SPARC_REMOTE_PATH`.
* `/bin/sh` is the Bourne shell: no `$( )`, no `local`, no `[[ ]]`. `bash` is
  **3.00** — no `mapfile`, no `${var,,}`, no associative arrays.
* Grouping matters: `a || b | while read …` parses as `a || (b | while …)`.
  That is exactly how `rd-session.sh stop` came to kill nothing.

**mrustc / minicargo**

* **minicargo does not honour `cargo:rerun-if-changed`.** An edited `.c` links
  against the stale archive and the build *succeeds* — several measurements
  were of the old object before this was found. `build-sparc.sh` drops the
  build-script cache when a shim is newer.
* mrustc defaults to `panic=abort`.
* `cc-rs` does not know this triple, and gcc on Solaris defaults to 32-bit
  sparc, so the wrapper pins `-m64`. Without it the linker says "wrong ELF
  class: ELFCLASS64" about an object that looks fine.
* `STD_ENV_ARCH` must be passed for a cross build or `std::env::consts::ARCH`
  reports the host's.

**Sockets**

* **Solaris 10 has no `SO_RCVTIMEO` and no `SO_SNDTIMEO`,** exactly as IRIX has
  none — and its headers say otherwise. `sys/socket.h` defines `SO_RCVTIMEO` as
  4102 and `SO_SNDTIMEO` as 4101, because those numbers are in the Solaris
  **11** ABI; the Solaris 10 kernel rejects both with **ENOPROTOOPT**, on TCP
  and UDP alike, while `SO_REUSEADDR` on the same socket succeeds.
  `probes/rcvtimeo.c` is that test, control included:

  ```
  SO_RCVTIMEO = 4102, SO_SNDTIMEO = 4101
  on a TCP socket:
    SO_RCVTIMEO  setsockopt FAILED: errno 99 (Option not supported by protocol)
    SO_SNDTIMEO  setsockopt FAILED: errno 99 (Option not supported by protocol)
  on a UDP socket:
    SO_RCVTIMEO  setsockopt FAILED: errno 99 (Option not supported by protocol)
  control: SO_REUSEADDR set OK -- so setsockopt itself works here
  ```

  This was assumed the other way for the whole port, and it cost the first
  end-to-end run. Rust's `set_read_timeout` is a thin wrapper on that
  setsockopt, and `session.rs` applied it with `?` on the non-IRIX path, so
  **every session ended the instant the video pump started** — the peer logged
  in, the encoder came up, and then:

  ```
  [  6.211] INFO  peer 'testpeer' (testpeer) logged in -- entering message loop
  [  6.213] INFO  encoder: 640x512, 375 kbps, 1 thread(s) of 1 processor(s)
  [  6.220] INFO  video: 1280x1024 framebuffer served at 640x512 (1/2)
  [  6.221] WARN  session ended: Option not supported by protocol (os error 99)
  ```

  which is the same failure, with the same symptom, that the IRIX arm of that
  code exists for and describes in its own comment. Solaris now shares every one
  of those arms: `session.rs` (the peer socket, and the input drain's pacing),
  `rendezvous.rs` (the UDP registration socket, and its resend wait), and
  `sys::wait_readable{,_fd}`. `http.rs` never needed changing — it already logs
  and continues on every platform, for this exact reason.

  The general shape, and the reason this belongs here: **a constant being
  defined on this machine says nothing about the option working on it.** Ask
  the kernel.

**Libraries**

* `libc` binds `sysconf` and `pthread_kill` to `__sysconf_xpg7` and
  `__pthread_kill_xpg7`, which arrived in **Solaris 11.4**. `src/compat_shim.c`
  forwards them. Every other `link_name` symbol libc uses does resolve here —
  the way to check is to ask the linker, not `nm`.
* libsodium is built with the stack protector and its runtime lives in gcc's
  `libssp` here, not libc. Linked statically from `/opt/csw/lib/sparcv9`.
* zstd needs `ZSTD_NO_ASM=1` or it assembles x86-64.
* libvpx needs GNU `diff` on PATH, `LD` set as well as `CC`, its helper-script
  shebangs pointed at bash, and its one C++ file (the RTC rate controller,
  which the agent does not call) removed — `pkgutil -i gcc5g++` is the
  alternative to removing it.

**X**

* `XFixesCursorImage.pixels` is `unsigned long *` — **8 bytes per pixel** here,
  holding a 32-bit ARGB value. Read as packed u32 and every second pixel is
  wrong.
* Xlib's default protocol-error handler calls `exit()`. Both shims install one
  that reports and continues; a rejected input event must not end a session.
* A damage object on the root **does** report child windows here, but do not
  assume it: the capture path checks itself against the canvas every two idle
  seconds and downgrades permanently if damage was lying. A synthetic
  startup probe was tried and answered wrongly on both servers it ran against.
* Default `xclock` redraws once a *minute*. Use `-update 1` when testing
  damage, or conclude the wrong thing (this happened).

---

## 9. File map

```
rustdesk-sparc-agent/
  src/capture_shim.{c,h}   MIT-SHM capture, DAMAGE, the hash fallback
  src/capture.rs           its Rust side, including the fused to_i420_rect
  src/input_shim.c         XTEST injection, from the IRIX shim via libXtst
  src/cursor_shim.c        XFIXES cursor
  src/clipboard_shim.c     X selections, both directions
  src/compat_shim.c        the two Solaris 11 symbols this machine lacks
  src/lib.rs               what is wired, and what is not, with reasons
  src/bin/captest.rs       capture, alone
  src/bin/prototest.rs     the portable half, checked against bytes
  src/bin/convtest.rs      the fused conversion, against the shared converter
                           and against named colours. Needs no display.
  probes/                  sysprobe.sh, xprobe.c, shimtest.c, inputtest.c,
                           cursortest.c, cliptest.c, rcvtimeo.c, smoke.rs
  scripts/build-sparc.sh   build the agent (needs SPARC_HOST)
  scripts/fetch-deps.sh    their sources, on a host that can verify a download
  scripts/build-deps.sh    the C libraries, on the Blade
  scripts/rd-session.sh    the desktop the agent serves
  scripts/sparc-cc-remote.py   the ssh "compiler"
  scripts/recompile-objects.sh replay codegen after a compiler change
```

The portable modules are **not** copied here: they come from
`../rustdesk-ppc-agent/src` by `#[path]`, as the IRIX port takes them.
