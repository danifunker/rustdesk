# rustdesk-irix65-agent

A RustDesk **agent** — the controlled side — for IRIX 6.5 on SGI MIPS, built for
the n32 ABI.

It shares almost all of its code with [`../rustdesk-ppc-agent`](../rustdesk-ppc-agent):
the protocol, framing, crypto, encoding and session logic are the same files,
included by `#[path]` rather than copied. What lives here is the part IRIX needs
of its own — screen capture over SGI's X extensions, input injection over XTEST,
and the toolchain and harness to build and test it.

**Status: it works, and it has never run on real hardware.** A peer connects
over the real protocol, logs in, and receives VP8 video; mouse injection is
verified. Everything measured so far was measured under emulation, on an
emulated R5000 or R4400 Indy. See *What is known and what is not* below before
trusting any of it.

## What it does

- **Capture through SGI-SCREEN-CAPTURE.** This X server predates the DAMAGE
  extension by a decade, but it ships SGI's own damage tracker, and
  `SGICapQueryCopyAndReset` returns the changed rectangles *and* their pixels in
  one round trip. An idle poll costs 2–4 ms. The PowerPC port had to sample
  every eighth row and hash it to guess at the same answer.
- **32-bit pixels from an 8-bit screen.** ReadDisplay resolves the colormap
  itself, so the capture path never touches a palette. Only the plain-Xlib
  fallback does.
- **The cursor composited for free.** `XRD_READ_POINTER` is honoured, so the
  agent sets `cursor_embedded` and never sends a cursor shape — which retires
  the synthetic-arrow workaround the Mac port needed.
- **Input over XTEST**, issuing the protocol requests directly rather than
  linking `libXtst` (see below for why that is not optional).

Three capture paths exist — damage, ReadDisplay, and plain `XGetImage` — and the
fallbacks are exercised deliberately by `capture-selftest`, not left as untested
code for the day the extensions turn out to be missing.

## Connecting to it

```sh
rustdesk-agent --password <PASSWORD>     # once
rustdesk-agent                           # listens on 0.0.0.0:21118
```

Then type the machine's **IP address into a RustDesk client's ID field**, no
port. Same as the PowerPC agent — see its README for the details of why that
works and what the client does with it.

Note the agent prints `mode: direct-IP, UNENCRYPTED` at startup when no
rendezvous server is configured. That is accurate, and worth knowing before
running it across a network you do not control.

## What is known and what is not

Verified on IRIX 6.5.22m under emulation:

| | |
|---|---|
| Protocol handshake, login, `PeerInfo` | works |
| VP8 video delivered to a peer | works |
| Mouse injection — absolute, relative, clamping | works |
| Capture: all three paths, rectangles checked against canvas pixels | works |
| Portable modules — crypto, config, png, json, http, protobuf, zstd | 20/20 checks pass on hardware |
| Keyboard injection into a focused Motif field | works |
| The settings panel's Start / Stop / Apply, pressed by injected input | works |
| The package installs with `inst` and runs with no environment set | works |

Not established:

- **Never run on real hardware.** Every number here is from an emulator, which
  is roughly 3x slower than a real R5000 and is not a graphics-accurate model.
- **Everything was tested at depth 8.** An O2 or Octane is likely 24-bit.
  ReadDisplay should normalise that, but the fallback and the converters have
  only ever seen 8-bit.
- **The agent crashes when its X server dies**, two different ways. On this
  platform that is not an edge case; see `RESUME.md`.
- **No login session.** Every measurement was taken against a bare
  `Xsgi :0 -bs -c`; xdm's server wedges under the emulator, so a 4Dwm desktop
  has never been captured. See `RESUME.md` §A REAL LOGIN SESSION — the
  access-control question that worried us turned out fine.

Since fixed and no longer on this list: the colours (they *were* swapped, and
the check that proves they are not now is three coloured xterms decoded by a
peer), throughput (5-20 fps for ordinary interaction, 1.3-1.9 for a full
repaint), and keyboard injection, which the settings panel finally gave
something to type into.

## The emulator's X server wedges, twice over

Under capture load, `Xsgi` used to stop making syscalls entirely: frozen CPU
time in `ps`, every client hanging, IRIX's own included. **That one is fixed
upstream** — iris `02c4e155`, "fix hostr readback issues" — and
[`docs/ISSUE-rex3-wedge.md`](docs/ISSUE-rex3-wedge.md) plus
[`docs/REX3-WEDGE-PROMPT.md`](docs/REX3-WEDGE-PROMPT.md) are kept for the
evidence and for the four things that turned out **not** to cause it.

A second one is still there: the server **xdm** starts wedges the same way, so a
logged-in desktop has never been captured. [`docs/ISSUE-xdm-wedge.md`](docs/ISSUE-xdm-wedge.md)
reports it. And a bare server can still wedge on its own — it did once during
this session's GUI work, which cost fifteen minutes because a wedged server
looks exactly like a script that never started. `/root/restart-x.sh` on the
guest is the way back, and anything that touches the display should check
`xdpyinfo` answers before blaming itself.

Real hardware may well not have any of it — which is the main reason to want a
test on a real machine.

## Talking to the emulated guest

The telnet forward stalls after a few dozen sessions: still accepted, nothing
ever comes back, and it is indistinguishable from a wedged guest.
[`docs/ISSUE-nat-inbound-stall.md`](docs/ISSUE-nat-inbound-stall.md) has what
was ruled out. **Anything automated should use the serial console**
(`iris-ci run --shell sh`), which has never failed; `scripts/ci-lib.sh` wraps
it as `guest_run`.

## Installing it on a real machine

There is a package now, and it installs the ordinary IRIX way:

```sh
# Software Manager / inst
inst -f /path/to/unpacked-tardist        #   install standard
                                         #   go

# or, without inst
gunzip -c rustdesk-agent-VERSION-n32.tar.gz | tar xf -
cd rustdesk-agent-VERSION-n32 && sh install.sh
```

It needs **nothing else on the machine**: libsodium, libvpx, mbedTLS and zstd
are linked in, everything else it uses ships with IRIX 6.5, and the one library
that does not -- `libgcc_s.so.1` -- is in the package and is found through an
rpath, with no environment variable and no wrapper. A machine that has never
heard of SGUG-RSE or a cross toolchain runs it.

```
/usr/local/sbin/rustdesk-agent                    the agent
/usr/local/sbin/rustdesk-agent-gui                the Motif settings panel
/usr/local/lib/rustdesk-agent/agent-helper.sh     start, stop, and every setting
                                            (`agent-helper.sh setup` asks for each)
/usr/lib/X11/app-chests/RustDesk.chest      a Toolchest entry
```

Building the package:

```sh
scripts/release.sh --boot                  # boots its own IRIX guest, disposes of it
scripts/release.sh --no-inst               # no guest at all: binaries + tarball
```

`docs/PACKAGING.md` is the whole pipeline: where the licensed boot image comes
from and how it stays private, the guest the run boots and throws away without
ever writing to that image, and the one step that can only happen inside IRIX.

## This directory is not standalone

The portable modules are included from the PPC tree by `#[path]`:

```
json convert zstd_frame frame http png crypto config sys encode
cursor clipboard input session api lan rendezvous capture
```

Those are relative paths to `../rustdesk-ppc-agent/src/`. **The two directories
must stay siblings.** Cloning one without the other will not build, and the
error you get names `-lrust_irix_compat` or a missing file rather than the
cause.

That is deliberate: a copy would drift, and the point of sharing the files is
that a protocol fix lands in both agents at once.

## Layout

```
src/                    the IRIX backend — the part that would ship
  capture_shim.{c,h}      three capture paths, shm canvas, downscale
  capture.rs              the Rust side, keeping the PPC agent's BANDS API
  input_shim.c            keyboard and pointer injection over XTEST
ports/rust/             the build: target spec, compat archive, crate harness
  agent-portable/         where rustdesk-agent and the test binaries are built
  env.sh                  private RUSTUP_HOME/CARGO_HOME and build vars
  build-compat.sh         the compat archive; run once per clone
ports/iris-run/         emulator harness: iris.toml, nvram, gsh.py
  guest/                  what runs on the guest; gui-press.sh presses buttons
gui/                    the Motif settings panel and its helper
  gui_motif.c             the window, and nothing that decides anything
  agent-helper.sh         everything that decides anything
scripts/                the build and packaging pipeline -- see docs/PACKAGING.md
inst/                   the inst(1M) product description, stamped at build time
desktop/                the Toolchest fragment
probes/                 the C probes behind every claim in RESUME.md
  xpoke.c                 aim the agent's own injection shim at a coordinate
tools/                  fix-sgi-archive.py, for SGI's static archives
docs/                   the wedge reports, the handover, and PACKAGING.md
BUILD.md                how to build it
RESUME.md               measurements, operating notes, and mistakes worth keeping
```

## Testing without a RustDesk client

`testpeer` speaks the real protocol — the same handshake `session.rs`'s own
tests use, which is the same one upstream's client uses:

```sh
testpeer 127.0.0.1:21118 <PASSWORD> 120
```

It reports frames, bytes, keyframes and cursor messages, and says plainly
whether video arrived. `capture-selftest` and `pipeline` exercise the capture
module and the whole capture→scale→convert→encode chain with per-stage timings.

## Further reading

- [`BUILD.md`](BUILD.md) — prerequisites, the toolchain, and the build
- [`RESUME.md`](RESUME.md) — what was measured, how the emulator behaves, and
  the mistakes that cost time
- [`../rustdesk-ppc-agent/README.md`](../rustdesk-ppc-agent/README.md) — the
  protocol side, which is shared
