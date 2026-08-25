# rustdesk-sparc-agent

A RustDesk *agent* (the controlled side) for **Solaris 10 on SPARC**, aimed at a
Sun Blade 2500 (UltraSPARC IIIi, XVR graphics). Third in the line after
`rustdesk-ppc-agent` (Mac OS X 10.4/10.5 on PowerPC) and `rustdesk-irix65-agent`
(IRIX 6.5 on MIPS), and it inherits from both:

* **From the PowerPC agent, the toolchain.** There is no rustc backend that can
  target Solaris 10 usefully, so the route is mrustc: Rust in, C99 out, and an
  old gcc on the target turns that C into SPARC objects.
* **From the IRIX agent, the platform layer.** Its capture and input shims are
  already X11 (`XGetImage`/MIT-SHM, XTEST), which is what Solaris 10 offers,
  and Xsun has DAMAGE and XFIXES besides. The Mac agent's framebuffer code has
  no equivalent here.

Nothing in this directory builds an agent yet. What is here is the groundwork:
the compiler wrapper the build will run through, and the probes that decide the
capture design.

## Where the port stands

**mrustc can already transpile the whole Rust standard library for
`sparcv9-sun-solaris`.** In the mrustc tree (branch `sparc-solaris-10`):

* `src/trans/target.cpp` gained `ARCH_SPARC64` and the `sparcv9-sun-solaris`
  target spec — big-endian, 64-bit, all atomic widths (SPARCv9 has `cas`/`casx`
  for 4 and 8 bytes and gcc synthesises the 1/2-byte ones from a word CAS loop),
  `-lsocket -lnsl -lrt` at link time, and none of the GNU archive-group or
  section-gc flags, which Solaris `ld` does not have.
* `#[repr(align(N))]` on a **union** is now implemented (AST → HIR → layout →
  metadata). Solaris's libc declares `pad128_t`/`upad128_t` that way, so without
  it libc does not parse at all. This was the only mrustc gap the stdlib hit.
* `script-overrides/stable-1.74.0-solaris/` carries the build-script outputs,
  with `STD_ENV_ARCH=sparc64`.

Building the libraries needs **no patches to the rustc 1.74 source** — libc
0.2.148 gates its x86-only Solaris pieces correctly, and std's Solaris paths
reach the Solaris 11 symbols (`pthread_setname_np`) through `weak!`, which is
exactly what a Solaris 10 target needs.

```sh
cd ~/repos/mrustc
MRUSTC_TARGET_VER=1.74 SPARC_HOST=user@blade SPARC_CC=/opt/csw/bin/gcc-5.5 \
CC_sparcv9_sun_solaris=~/repos/rustdesk/rustdesk-sparc-agent/scripts/sparc-cc-remote.py \
  make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 \
       MRUSTC_TARGET=sparcv9-sun-solaris OVERRIDE_SUFFIX=-solaris \
       STD_ENV_ARCH=sparc64 PARLEVEL=2
```

**Rust programs now build and run on the Blade.** `probes/smoke.rs`, linked
against that stdlib and run on Solaris 10:

```
os=solaris arch=sparc64 pointer_width=64
native bytes of 0x01020304 = [01, 02, 03, 04] (big-endian)
u128 align=16 big/7=16203922234330403022065457496750867212
atomics: u8=232 (expect 232 after wrap) u64=1000 (expect 1000) mutex=1000 (expect 1000)
catch_unwind: unwound OK
slept 120.063999ms; unix time 1787659147s
bound 127.0.0.1:33385
file round-trip: written by rust on solaris
ALL SECTIONS COMPLETED
```

Threads, sub-word atomics (the ones gcc synthesises from a word CAS), Mutex,
DWARF unwinding through `catch_unwind`, clocks, a TCP bind and file I/O all
behave. Note the `-C panic=unwind`: mrustc defaults to `panic=abort`, and with
the default a panic aborts the process rather than unwinding.

Four mrustc changes were needed to get here, all on `sparc-solaris-10`:
`#[repr(align(N))]` on unions; `-std=gnu11` in the target spec; `STD_ENV_ARCH`
made overridable so a cross build stops reporting the host's arch (it said
`arch=x86_64` on SPARC -- hence `STD_ENV_ARCH=sparc64` in the command above);
and a format-specifier fix where `{:02x?}` left `?}` in the output because the
parser never stepped over the `x?`.

## The machine, measured

Sun Blade 2500, Solaris 10 8/11 (s10s_u10wos_17b), **one** UltraSPARC IIIi at
1600 MHz, 4 GB RAM, 15 GB free. XVR-600 framebuffer at `/dev/fbs/jfb0`.

**Compiler.** `/opt/csw/bin/gcc` is 4.9.2 and gets most of the way: C11,
`__int128` (16-byte aligned), big-endian sparcv9 output -- but only with `-m64`
(32-bit sparc has no `__int128`) and only with `-std=gnu11` (it still defaults
to gnu89, which rejects the C99 declarations mrustc emits; the target spec now
passes it). What 4.9 does *not* have is `__builtin_add_overflow` and its `sub`
and `mul` siblings -- those arrived in gcc 5. mrustc emits them for Rust's
checked arithmetic, so 4.9 compiles them as implicit declarations and they turn
up as the **only** three undefined symbols in a full link:

```
Undefined                       first referenced in file
__builtin_add_overflow          /var/tmp//cc86bhv3.o
__builtin_sub_overflow          ...
__builtin_mul_overflow          ...
```

Everything else -- all of libstd, libc, the unwinder, `-lsocket -lnsl -lrt` --
resolved. `gcc5core` (5.5.0) is now installed at `/opt/csw/bin/gcc-5.5` and has
them, so that is what `SPARC_CC` should point at. Rebuilding all 20 stdlib
objects through it takes about 9 minutes (libcore 222 s, libstd 123 s).

`cc1` is a 32-bit SPARC32PLUS binary, so it is capped near 4 GB of address
space however much RAM the machine has. It has not been a problem yet: a 3.8 MB
translation unit compiles in 21 s and the 31 MB one goes through, but
`SPARC_BIG_TU_BYTES` in the ssh wrapper is the lever if it becomes one.

**Display.** `Xsun :0 -defdepth 24 -nolisten tcp -auth /var/dt/A:0`, started by
`dtlogin`. Nobody is logged in graphically, so `dtgreet` is on screen -- and it
holds an X server grab, which means a client does not get an auth failure, it
*hangs inside `XOpenDisplay`* until someone logs in. Confirmed with `pstack` on
a stuck probe: blocked in `_XWaitForReadable` during the connection handshake.

The extensions are better than they first looked. `Xsun` itself advertises:

```
DAMAGE  XFIXES  MIT-SHM  XTEST  RENDER  SHAPE  RECORD  SYNC
XKEYBOARD  DOUBLE-BUFFER  SUN_OVL  XC-MISC  MIT-SUNDRY-NONSTANDARD
```

(read out of the binary with `strings /usr/openwin/bin/Xsun`, since the server
is not in the open-source drop and the display cannot be reached yet). So the
agent can follow the IRIX design -- server-reported damage rectangles and a
real cursor shape -- rather than the PowerPC port's sampled-row change
detection and drawn-on arrow.

The client libraries are split across two prefixes, which is what made XFIXES
look absent at first: Xlib/Xext/Xtst are in `/usr/openwin/lib`, but
**`libXfixes` and `libXdamage` are in `/usr/openwin/sfw/lib`** -- both with
64-bit builds in `sparcv9/` -- while their headers sit with all the others in
`/usr/openwin/include/X11/extensions`. The Solaris 10 open-source drop is what
settled it: `X/src/packages/SUNWxwplt/prototype_com` lists
`openwin/sfw/lib/$plat_64/libXfixes.so.1` explicitly.

**Solaris things that bit, worth knowing before writing another probe.**
`/usr/bin/grep` has no `\|` alternation (use `egrep`) -- a pattern that looked
for the X server silently matched nothing and made a running Xsun look absent.
There is no `timeout`, no `head -c`, and no `grep -q`. `pkill -f xprobe` kills the ssh command
running it, because that command line contains the pattern. And an ssh command
runs a non-login shell whose PATH has neither `/opt/csw/bin` nor rsync in it,
which is why the wrapper sets `SPARC_REMOTE_PATH`.

### Why sparcv9 (64-bit) and not 32-bit sparc

rustc has no 32-bit SPARC/Solaris target and the libc crate has no arch module
for one, so a 32-bit build would mean writing libc's Solaris/SPARC support from
scratch. `sparcv9-sun-solaris` is a tier-2 rustc target, and libc already
carries its quirks (the 8-byte `cmsg` alignment, the `addrinfo` pad). The Blade
runs a 64-bit kernel, so the only cost is that every C library the agent links
must also be 64-bit — see the `sysprobe.sh` output for whether
`/usr/openwin/lib/sparcv9` has X11.

## Probes — run these on the Blade first

```sh
scp probes/sysprobe.sh blade:/tmp && ssh blade 'sh /tmp/sysprobe.sh'
```

Reports the machine, and — the part that gates everything — which compilers are
installed and whether any of them can build C11. mrustc's output needs
`<stdatomic.h>`, `__int128` and `_Static_assert`; Solaris 10's own
`/usr/sfw/bin/gcc` is 3.4.3 and has none of them, so a newer gcc (OpenCSW
`gcc5core`, or one built on the box) is a prerequisite, not a preference.

```sh
scp probes/xprobe.c blade:/tmp
ssh blade 'gcc -O2 -I/usr/openwin/include -o /tmp/xprobe /tmp/xprobe.c \
             -L/usr/openwin/lib -R/usr/openwin/lib -lXext -lX11 && DISPLAY=:0 /tmp/xprobe'
```

Reports the X server's identity and extensions, the exact pixel layout of a
full-screen grab (raw bytes, not an interpretation — the IRIX port found A,B,G,R
where the Mac had A,R,G,B), and how long `XGetImage` and `XShmGetImage` take.
That last number is the frame-rate ceiling before any encoding, and it decides
whether the agent polls or waits on DAMAGE.

## Compiling on the Blade over ssh

`scripts/sparc-cc-remote.py` is a drop-in `cc` that ships each `.c` to the
machine, runs gcc there and copies the `.o` back, so minicargo's dependency
graph and parallelism keep working:

```sh
export SPARC_HOST=user@blade
export SPARC_CC=/opt/csw/bin/gcc          # whatever sysprobe.sh found that does C11
export CC_sparcv9_sun_solaris=$PWD/scripts/sparc-cc-remote.py
```

A `sparc-sun-solaris2.10` cross-gcc on the Linux box (against a sysroot copied
off the Blade, as `/opt/irix-sysroot` is for the IRIX port) is the eventual
answer for build times. The ssh wrapper is what proves the C compiles and links
at all, and it needs nothing but ssh.

## Next

1. **A graphical login on the console**, so `dtgreet` releases its grab and
   `probes/xprobe.c` can report what Xsun does with `XGetImage`/`XShmGetImage`.
   Until someone logs in, every X client hangs in `XOpenDisplay`.
2. Fork `rustdesk-ppc-agent` here, add `target_os = "solaris"` arms alongside
   the existing `macos`/`irix` ones, and port the IRIX X11 shims.
3. libsodium for sparcv9 (the PPC and IRIX ports both build it from source);
   VP8 can wait -- start with the raw/PNG/zstd frame paths.
