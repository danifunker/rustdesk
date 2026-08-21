Continue the IRIX RustDesk agent port. Read
`/home/dani/repos/rustdesk/rustdesk-irix65-agent/RESUME.md` first — it is the source of
truth for state, environment, what is already verified, what has been ruled
out, and the mistakes not to repeat. Do not re-derive anything it records.

You have a long uninterrupted window. Work autonomously and go deep. I will not
be around to answer questions, so make reasonable calls and write down the
assumption rather than stopping to ask.

Priorities, in order:

1. **Unblock X.** Cross-compiled X11 clients cannot complete the X connection
   setup while IRIX's own clients can. RESUME.md §THE BLOCKER has the syscall
   trace, the ruled-out list, and the decisive first test — compiling
   `probes/minimal.c` natively on the guest with nekoware gcc. Run that test
   first; it halves the search space.

2. **If X comes up, build the capture module** — `XShmReadDisplayRects` for
   dirty rects, `XRD_READ_POINTER` for the composited cursor, `XShmGetImage`
   fallback exercised deliberately, downscale path built in from the start.
   Verify the in-memory byte order before writing any converter.

3. **If X resists for more than about two hours, switch to the Rust n32
   toolchain** (RESUME.md §C). It needs no X, it is on the critical path
   regardless, and it is a big chunk: get mogrix's Rust cross-compilation
   working and the agent's portable modules — crypto, config, png, zstd_frame,
   convert, json, http, protos — compiling for `mips-sgi-irix6.5`. Come back to
   X afterwards.

Boundaries:

- **Do not modify `~/repos/iris`.** I am handling iris in a separate session.
  Note anything you wish were different instead.
- **Do not start IRIX 5.3.** Deferred by decision.
- **Fold CHD changes into `~/Indy-IRIX65_dev.chd`** using the chdman procedure
  in RESUME.md, and verify by extract-and-mount before swapping. Keep
  `Indy-IRIX65_dev.chd.bak-before-first-write`. Tell me in your summary exactly
  what changed on the disk.
- Keep mogrix work on the `danifunker-ports` branch. Rules and patches only —
  no `add_rule` bookkeeping, since the MCP server is not connected.

When you finish or run out of road, update `RESUME.md` in place so the next
session starts where you stopped, and give me a summary that says plainly what
worked, what did not, and what you are unsure about. If something is still
broken, I would rather have an accurate description of the failure than a
hopeful one.
