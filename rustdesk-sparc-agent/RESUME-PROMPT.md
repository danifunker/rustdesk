Continue the Solaris 10 / SPARC RustDesk agent port. Read
`/home/dani/repos/rustdesk/rustdesk-sparc-agent/RESUME.md` first — it is the
source of truth for the machine, the toolchain, what is already measured, what
is deliberately *not* shared with the other two ports, and the traps that cost
time once. Do not re-derive anything it records.

**The agent runs.** It serves a peer, the picture is the right colour, and §5
has the frame rate and where each frame's time goes. What is left is polish,
proof against something that is not ours, and packaging.

The Blade is at `192.168.99.176` as `dani` and ssh keys work. There is no root:
ask for anything that needs it, and say exactly which package or command. The
agent's desktop is `scripts/rd-session.sh start` (Xvfb `:2` + dtwm + dtterm) —
the console `:0` still needs root and is blocked behind CDE's greeter grab.

Priorities, in order:

1. **`screen_content: 2` is probably costing bandwidth for nothing.** RESUME.md
   §5a has the sweep: on its own it is slower than baseline *and* nearly
   doubles the bytes. But the sweep varies one knob from the baseline `Tune`,
   so it never measures `profile 3 + screen_content 2`, which is what
   `video_tune()` actually sets. Measure that combination before deleting
   anything. `static_threshold 30000` is the other row worth a look.

2. **A real RustDesk client, not `testpeer`.** Everything proved so far is the
   protocol against an implementation of the same protocol, written from the
   same notes. A stock client is the thing that finds where they differ, and
   nothing else will. Direct-IP to `192.168.99.176:21118`; the password is
   `sparctest` as the last run left it, `--show-key` prints the identity, and
   `ipfilter` is disabled so nothing is in the way. Say plainly whether it
   works, including what looked wrong.

3. **The Motif panel** (§6.3) and **packaging** (§6.4). Solaris wants a
   `pkgadd` datastream; the IRIX port's `inst/` and release script are the
   model.

Loose ends worth folding in when you are near them — the full list is §7: no
scroll wheel (three-button pointer), INCR clipboard transfers reported as
empty, `rd_clip_changed` returning 0 where the shared contract says -1, the
agent's Rust half compiling at `-O0` because the unit is ten times
`SPARC_BIG_TU_BYTES` (and cc1 having looked like it has headroom), and the
PowerPC `output-*` trees left stale by the union metadata change.

How to work:

- **Measure on the machine.** Every number in RESUME.md came from a probe run
  on the Blade, and the assumption that cost the most — that Solaris 10 has
  `SO_RCVTIMEO` — was believed because the header defines the constant. If you
  find yourself reasoning about what Solaris probably does, write a probe
  instead; `probes/` is full of the pattern and `probes/rcvtimeo.c` is what
  that one should have been from the start.
- Build a piece, prove it alone, then wire it. That is why capture, input,
  cursor and clipboard each have their own C driver, and why `convtest` needs
  no display.
- When a build "succeeds" but behaves like older code, suspect the build-script
  cache before suspecting yourself (§8).
- A build cycle is 15–20 minutes now that the whole crate is in it. Audit the
  cfg gaps statically before building rather than discovering them one per
  cycle; `grep` for arms naming `macos` or `irix` without `solaris`.

Boundaries:

- **The shared PowerPC sources are three ports' code.** Adding
  `target_os = "solaris"` arms is fine and expected — and check the negated
  form, since `cfg(any(…))` and `cfg(not(any(…)))` are different strings.
  Changing what macOS or IRIX *does* is not. If a shared file needs
  restructuring, say so rather than doing it quietly.
- **Keep mrustc changes additive** and on `sparc-solaris-10`. Anything that
  changes rlib metadata invalidates every `output-*` tree, including the
  PowerPC ones — say so loudly if you do it again.
- Do not disable the capture path's self-check or the input shim's error
  handler to make something work. Both exist because of a specific failure.

When you finish or run out of road, update `RESUME.md` in place so the next
session starts where you stopped, and give a summary that says plainly what
worked, what did not, and what you are unsure about. An accurate description of
a failure is worth more than a hopeful one.
