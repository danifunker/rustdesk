# Resume prompt: finish the IRIX settings panel

Continue the IRIX RustDesk agent. Read `rustdesk-irix65-agent/RESUME.md` first —
it is the source of truth for state, environment, what is verified, what is
ruled out, and the mistakes not to repeat. Do not re-derive anything it records.

**The agent is fast enough to use, its self-hosted infrastructure support works,
and the Motif settings panel is written and running.** What is left is polish,
one untested path, and wiring the whole thing to Dani's own deployment.

## Where it stands

**It works.** `gui/gui_motif.c` (about 800 lines) plus `gui/agent-helper.sh` and
`gui/build-gui.sh`, cross-built with the ordinary toolchain and seen running on
the emulated Indy under 4Dwm. From a screen capture, not inferred:

```
File   Agent                                   Help
This machine
  Agent ID:   ff6izj02b
  Status:     stopped
  Capture:    (not yet -- start a session)
  Password:   [hunter2            ] [Set]
Infrastructure
  ID server:         [                    ] [Apply]
  Server key:        [                    ] [Apply]
  Relay (optional):  [                    ] [Apply]
  Console (optional):[                    ] [Apply]
  CA bundle (optional):[                  ] [Apply]
[Start] [Stop] [Restart] [Refresh]
The agent is stopped.
```

- SGI scheme, menu bar with Help right-aligned, XBM window icon.
- Reads the agent's real state through the helper on open and on Refresh.
- Stop is greyed while the agent is stopped; Start is greyed while it runs.
- The status line reports in the helper's own words.
- The Capture row reports the capture path once a session has run, because an
  agent on the ReadDisplay fallback works and is slow enough to look broken,
  and this is the only place a person would find that out.

The helper half is verified independently, which is where all the behaviour is:
`set server`, `set key` and `set api` persist to the config and read back
through `status`, and clearing each one works (`set server ""` runs
`--no-server`).

**What has not been done is a click.** The panel's read path is proven — the
window is showing real values that came through `popen()` from the helper — and
Apply uses the same function with different arguments, but no button has been
pressed by a human or by injected input. Pressing one is the first thing to do.

## The layout, and why it looks the way it does

Worth reading before changing it, because three plausible approaches do not work
here and the failure is silent:

- **`XmForm` with edge attachments** collapsed the whole panel to a 180-pixel
  stub. A Form whose children attach to both its edges is a circular size
  negotiation, and Motif resolves that by collapsing.
- **Forcing a shell geometry** on top of that was worse: correctly sized,
  completely empty, because the rows had already been laid out for the
  collapsed size.
- **`XmFrame` around each group** — with `XmNchildType`/`XmFRAME_TITLE_CHILD`
  (Motif 1.2 does have it, checked in `Xm.h`), and again with a plain heading
  above an untitled frame — gave its work area one row less height than the work
  area asked for. **The last row of every group was silently clipped**: the panel
  looked entirely right and simply had four rows where it should have five. A
  zero-height separator at the end absorbed it in a four-row group and not in a
  five-row one, so the shortfall is not a fixed number of pixels and padding
  around it is not a fix.

What is there now is `XmRowColumn` throughout, with a heading and a rule instead
of a frame, and every label a fixed width with `XmNrecomputeSize False` so the
columns line up. RowColumn computes its size from its children and has nothing
to negotiate. **If you reintroduce `XmFrame`, count the rows.**

## How to see what you have changed

**This is the part that costs time, so it is worth setting up first.** There is
no way to see the panel except by capturing the screen, and two of the obvious
routes do not work:

- `iris-ci screenshot` reads the REX3 framebuffer and **comes back all black**
  on this build, even when the screen visibly is not. It also did during the xdm
  investigation; do not trust it.
- The guest cannot push a file anywhere.

What does work, and is scripted as `ports/iris-run/guest/gui-shot.sh`:

```sh
# on the guest: start one panel, then capture with the agent's own capture path
sh /tmp/gui-shot.sh          # writes /tmp/frame.ppm

# on the host
iris-ci login root           # get: needs a shell on the serial console
iris-ci get /tmp/frame.ppm --to /tmp/frame.ppm --timeout 600
python3 -c "from PIL import Image; Image.open('/tmp/frame.ppm').save('/tmp/f.png')"
```

The capture is at 1/2 scale, which is plenty to read the labels.

## Two traps that cost real time here

**IRIX `ps -e` truncates the command to eight characters.** `rustdesk-agent-gui`
shows as `rustdesk-`, so `ps -e | grep rustdesk-agent-gui` matches **nothing** —
five consecutive launches looked like crashes when the panel had been running
the whole time. And `grep rustdesk` matches the agent *and* the panel, so a
careless kill loop takes the panel down with the agent. Use
`ps -e -o pid,args`, which carries the full command line. The scripts in
`ports/iris-run/guest/` have been fixed; anything you write needs the same care.

**Telnet stops answering after enough sessions, and it is inetd, not ptys.**
IRIX inetd disables a service that has been spawned too often in too short a
window and leaves the socket accepting — so the connection negotiates options
and then never prints a login prompt, which is indistinguishable from a wedged
guest. `who` and `ps` will both look fine, and `ps -e | grep -c telnetd` will
say 1. A HUP to inetd did not bring it back; a reboot did, every time.

Two things follow. **Batch aggressively** — put a whole sequence in one
`gsh.py` invocation rather than one call per command; a build-deploy-capture
cycle should be one session, not five. And **`iris-ci` over the serial console
keeps working when telnet does not**, so it is the way out rather than a
reboot: `iris-ci login root` then `iris-ci run '...'`. Watch for line-length
corruption there — a long `grep` output came back as several hundred bytes of
garbage followed by "name too long", which is the console mangling the line and
not the command failing.

## Building it

```sh
./gui/build-gui.sh          # writes gui/rustdesk-agent-gui
```

Two things the stock cross environment does not give you, both handled in the
script and both explained in its header:

- **The Motif headers are not in `/opt/irix-sysroot`.** The libraries are, and
  they are all *shared*, which is the only reason this is possible at all — LLD
  cannot read the static archives SGI ships, which is why `input_shim.c` issues
  XTEST protocol requests by hand. `/usr/include/Xm` on the machine is a symlink
  to `/usr/Motif-1.2/include/Xm`, so a plain `tar cf` of `/usr/include/Xm`
  stores a link and nothing else. Tar the real directory. This has been done
  already, so the sysroot has them; the note is for a fresh machine.
- **`-D_XmConst=`.** `Xm/XmStrDefs.h` declares `externalref _XmConst char ...`
  for SGI's keypad virtual keys inside a branch that never defines `_XmConst`.
  MIPSpro lets it through; clang stops. Defining it empty is the whole fix.

That is Motif **1.2.4**, which is what 6.5 ships and 5.3 carries, so keep to the
1.2 API and one source covers the family — the same rule `../irixscsitb`'s
`gui_motif.c` follows, and that file is the reference for everything here: the
`XtArgcType` typedef, the `useSchemes`/`SgiSpec` fallback resources, labels as
resources rather than string literals, the XBM window icon (IRIX 5.3 ships
`libXpm.so` with no header), the busy cursor with `XmUpdateDisplay`, and the
reusable error/info dialogs with Cancel and Help unmanaged.

## What to do first

1. **Press a button.** Everything else here is proven; this is not. Either sit
   at the machine, or drive it through the agent itself — start the agent,
   connect a peer, and inject a click at the Apply button's coordinates. The
   second is worth more than it sounds: `rd_key`/`rd_key_char` have never had a
   real keystroke put through them either, because until now the bare X server
   had nothing focusable on it, and the panel is the first thing that has been.
   See `RESUME.md` §Next steps item 4.
2. **Then wire it to Dani's deployment**, below.

## What is deliberately not done

- **A desktop icon.** A proper Indigo Magic one is FTR rules plus a vector
  `.icon` file — see `../irixscsitb/desktop/` for what that involves. The
  window-manager icon (iconified window) is done.
- **Installing anywhere.** The panel runs from `/tmp` like everything else here.
  `agent-helper.sh` already looks in `/usr/sgug/lib/rustdesk-agent/` first, so
  packaging is a matter of putting it there.
- **A password that echoes as bullets.** Motif 1.2 has no password widget and
  the usual trick is a `modifyVerify` callback that keeps the real text and
  echoes something else. The field currently shows the password in clear, which
  is what the Mac panel does too.

## After that

Dani wants to point this at their own infrastructure. The CLI side is done and
**verified on IRIX this session** — `--server` registers over UDP and
`--api-server` does a real TLS handshake, both against stand-ins on the build
host (`ports/iris-run/tls-endpoint.sh` and `guest/rendezvous-check.sh`). Both
were broken until this session and both by the same fault, so if a third path
starts failing on IRIX with `Option not supported by protocol (os error 99)`,
that is `SO_RCVTIMEO`; see §SELF-HOSTED INFRASTRUCTURE in `RESUME.md`.

What is needed from Dani to finish that: the hbbs hostname, the server key, and
the console URL with its CA if it is private.

## When you finish

Update `RESUME.md` in place, and say plainly what works and what does not. If
the layout is still wrong, an accurate description of what you tried is worth
more than a hopeful one — the list above is what two hours of it looks like, and
it is only useful because it is complete.
