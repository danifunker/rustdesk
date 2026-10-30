Continue the Windows NT 4.0 RustDesk agent, C-Desk-Vint for Windows NT.
Read `/home/dani/repos/rustdesk/crustdesk-winnt/RESUME.md` first -- it is
the source of truth for the build (sources staged here, compiled by Visual
C++ 4 on the NT machine), the design, what was proven on the PowerPC and
the traps; **section 6 says what is left**. Do not re-derive anything it
records. The core is shared with the classic Mac and IRIX agents
(`../crustdesk-macos-classic/src/core`): run `make -C
../crustdesk-macos-classic/host test` after touching it, and keep `make
selftest-host` and `-selftest` on the NT machine passing.

**Where it stands.** Built and tested on QEMU's IBM 40P with NT 4.0 SP2 for
PowerPC (the overlay on BigDisk, `scripts/qemu-40p.sh`): installed as a
service from its settings window, registered with rs-ny.rustdesk.com,
reached by ID through the public relay, the logon and locked screens and
Ctrl-Alt-Del, input, clipboard, files, screenshots, console https. x86, MIPS
and Alpha are not compiled yet.

What is open, for the user to choose from (ask; none is decided):

1. **Another processor**: which machine or emulator, and its Visual C++.
2. **The secrets on FAT** (section 6, item 2): registry with an ACL, or
   leave it documented.
3. **Speed** on real hardware, and whether a mirror driver is worth it.
4. **A release**: the PowerPC files from `make dist`; upload only when
   asked (notes are CRLF), and say it was tested under QEMU, not hardware.

Boundaries: shareable builds carry the public ID server and nothing of the
user's (`scripts/check-generic.sh` runs in `make dist`). The NT image is the
user's download; keep its disk read-only and work in the overlay. The disk
is nearly full: `build/` and scratch copies go when done. Commit to
`vintage-agents` with a `crustdesk-winnt:` subject in the house voice, with
the Co-Authored-By line; never push unless asked.

How to work: `scripts/qemu-40p.sh &` (ntexec starts at its automatic
logon), then `make nt` / `make install-nt` / `make selftest-nt`. Drive
sessions with `../crustdesk-macos-classic/host/cdvpoke.py 127.0.0.1:21118
PASSWORD ...` (Map mode: Windows scancodes; `ctrl 100` is Ctrl-Alt-Del) or
`rdvpoke.py ID PASSWORD ...` by ID; `save FILE` then `host/build/vp8dump`.
`scripts/ntvm.py shot` sees the NT screen itself. Say plainly what was tested
where, with which client.
