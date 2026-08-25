Continue the Solaris 10 / SPARC RustDesk agent port. Read
`/home/dani/repos/rustdesk/rustdesk-sparc-agent/RESUME.md` first — it is the
source of truth for the machine, the toolchain, what is already measured, what
is deliberately *not* shared with the other two ports, and the traps that cost
time once. Do not re-derive anything it records.

The Blade is at `192.168.99.176` as `dani` and ssh keys work. There is no root:
ask for anything that needs it, and say exactly which package or command. The
agent's desktop is `scripts/rd-session.sh start` (Xvfb `:2` + dtwm + dtterm) —
the console `:0` still needs root and is blocked behind CDE's greeter grab.

Priorities, in order:

1. **`Capturer::to_i420_rect`, and the three small methods beside it.**
   RESUME.md §4 has the signatures, the IRIX file and line numbers to work
   from, and the reason the IRIX inner loop cannot simply be renamed: it reads
   A,B,G,R and this machine's canvas is A,R,G,B. Get that wrong and every frame
   has red and blue swapped, which looks like a client bug. Check it against
   `convert::argb_to_i420` at `factor = 1` before believing it, and check a red
   patch and a blue patch specifically.

2. **Wire `session`, `clipboard`, `api`, `rendezvous` into `src/lib.rs`,
   uncomment the `rustdesk-agent` binary, and get it to build.** Expect more
   cfg gaps — §4c has the pattern that works and the one that bit (the negated
   `cfg(not(any(…)))` form is a different string from `cfg(any(…))`).

3. **Run it against a real client**, direct-IP, and say plainly whether it
   works. Nothing in this port is proved end to end yet: every piece is proved
   *alone*.

4. **Then measure and tune.** `DEFAULT_SCALE` and the encoder `Tune` are
   inherited from an emulated Indy on a guess. VP8 at 1280×1024 on a 1.6 GHz
   UltraSPARC IIIi may want the halved size, or may not.

5. **Then the Motif panel** (RESUME.md §5.3) and packaging (§5.4).

Loose ends worth folding in when you are near them — the full list is §6:
no scroll wheel (three-button pointer), INCR clipboard transfers reported as
empty, `SO_RCVTIMEO` on Solaris assumed rather than measured, and the PowerPC
`output-*` trees left stale by the union metadata change.

How to work:

- **Measure on the machine.** Every number in RESUME.md came from a probe run
  on the Blade, and two conclusions in this port were wrong until one was. If
  you find yourself reasoning about what Solaris probably does, write a probe
  instead — `probes/` is full of the pattern.
- Build a piece, prove it alone, then wire it. That is why capture, input,
  cursor and clipboard each have their own C driver.
- When a build "succeeds" but behaves like older code, suspect the build-script
  cache before suspecting yourself (§7).

Boundaries:

- **The shared PowerPC sources are three ports' code.** Adding
  `target_os = "solaris"` arms is fine and expected; changing what macOS or
  IRIX does is not. If a shared file needs restructuring, say so rather than
  doing it quietly.
- **Keep mrustc changes additive** and on `sparc-solaris-10`. Anything that
  changes rlib metadata invalidates every `output-*` tree, including the
  PowerPC ones — say so loudly if you do it again.
- Do not disable the capture path's self-check or the input shim's error
  handler to make something work. Both exist because of a specific failure.

When you finish or run out of road, update `RESUME.md` in place so the next
session starts where you stopped, and give a summary that says plainly what
worked, what did not, and what you are unsure about. An accurate description of
a failure is worth more than a hopeful one.
