# crustdesk-irix

**C-Desk-Vint for IRIX**: a RustDesk agent -- the controlled side -- for SGI
machines running IRIX 5.3 or later, built for the MIPS I instruction set, so
it runs on an R3000 IRIS Indigo. It is the classic Mac agent's portable core
(`../crustdesk-macos-classic/src/core`: the protocol, the encryption, the VP8
encoder) with an IRIX side of its own: X11 readback, XTrap damage reports,
XTEST input.

Work in progress; see RESUME.md for the state and the numbers.

## How it is built

There is no compiler for IRIX 5.3 newer than SGI's own 1994 C, so:

- **GCC 13 for `mips-linux-gnu`** compiles everything, for MIPS I / o32 / PIC
  (the SVR4 ABI IRIX 5 uses) against IRIX 5.3's own headers --
  `tools/irix-gcc`, with `tools/asbin/as` renaming the sections GCC files PIC
  data under (`.data.rel*`) to the ones SGI's linker knows.
- **`tools/irixobj.py`** makes each object acceptable to IRIX 5.3's `ld`:
  drops GNU-only sections, rebases the `.mdebug` symbolic header (it holds
  absolute file offsets), and reshapes the symbol tables the way IDO emits
  them (ELF locals are section symbols only; the `.mdebug` externals pair
  one-for-one with the ELF globals).
- **SGI's `ld` links it, on the IRIX machine** (`scripts/indigo.py link`),
  against the machine's own libc, libX11, libXext, libXtst and libXTrap.

```sh
scripts/fetch-toolchain.sh                      # GCC into build/toolchain
IRIX53_IMAGE=irix53.chd scripts/make-sysroot.sh # headers into build/irix53-sysroot
make                                            # objects into build/obj
INDIGO=host scripts/indigo.py link              # link there, fetch build/C-Desk-Vint
```

## The packages

`make dist` makes both, from the linked agent, with one SHA256SUMS:

- **`C-Desk-Vint-IRIX-5.3+-<build>.tardist`**, the inst product (`cdeskvint`,
  subsystem `cdeskvint.sw.base`): what IRIX's `inst` and Software Manager
  install, upgrade and remove. `scripts/make-inst.sh` sends `inst/` and the
  files to the IRIX machine and runs its `gendist` there (`inst_dev.sw`, on
  the IRIX 5.3 development CD); see `inst/README.md`.
- **`C-Desk-Vint-IRIX-5.3+-<build>.tar`**, the agent with `pkg/install.sh`
  and `uninstall.sh`, to install by hand.

`scripts/indigo.py` drives the IRIX machine over telnet (5.3 has no ssh): it
types one small file through the terminal -- `tools/irix/xfer.c`, built there
with `cc` -- and moves everything else over TCP with it.

## Known limits

- **24-bit displays are untested on a real SGI.** The agent chooses at run
  time (`capture=` in prefs): an 8-bit or 24-bit root through MIT-SHM, and
  windows of another visual or colormap -- IRIS GL RGB windows even on an
  Entry Indigo -- through SGI's ReadDisplay. The 24-bit decoding was checked
  against Xvfb (depth 24) over TCP; the 8-bit and ReadDisplay paths on the
  Indigo.
- **The pointer is a plain arrow on the client**, wherever IRIX's is; IRIX's
  own shapes (the text cursor, the watch, launch feedback) are not sent.
- The Indigo this was built on has no route off its LAN: connect by IP there,
  or give the machine a gateway and DNS to use an ID server.
