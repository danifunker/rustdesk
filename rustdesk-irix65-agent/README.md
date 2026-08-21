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

Not established:

- **Never run on real hardware.** Every number here is from an emulator, which
  is roughly 3x slower than a real R5000 and is not a graphics-accurate model.
- **Colours are unverified.** IRIX hands back **A,B,G,R** in memory; the
  converters came from a Mac port whose framebuffer was A,R,G,B. The C and Rust
  paths agree with *each other*, which proves only that they are consistently
  whatever they are. Red and blue swapped is the failure mode to look for.
- **Everything was tested at depth 8.** An O2 or Octane is likely 24-bit.
  ReadDisplay should normalise that, but the fallback and the converters have
  only ever seen 8-bit.
- **Keyboard injection is written but unproven** — the bare test server has no
  window manager and nothing focused to type into.
- **The agent crashes when its X server dies**, two different ways. On this
  platform that is not an edge case; see `RESUME.md`.
- **Throughput is unmeasured**, because of the blocker below.

## The blocker, which is not in this code

Under emulation the X server wedges after roughly one full frame: `Xsgi` stays
in `ps` with its CPU time frozen, makes no syscalls, and never reads from its
sockets again. Every client hangs, IRIX's own included. It reproduces on both
R5000 and R4400.

That is a fault below Xlib, in the emulator's Newport/REX3 model.
[`docs/ISSUE-rex3-wedge.md`](docs/ISSUE-rex3-wedge.md) is a report ready to file;
[`docs/REX3-WEDGE-PROMPT.md`](docs/REX3-WEDGE-PROMPT.md) is the fuller handover,
including the four things that turned out **not** to cause it. Real hardware may
well not have the problem at all — which is the main reason to want a test on a
real machine.

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
probes/                 the C probes behind every claim in RESUME.md
tools/                  fix-sgi-archive.py, for SGI's static archives
docs/                   the REX3 wedge report and handover
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
