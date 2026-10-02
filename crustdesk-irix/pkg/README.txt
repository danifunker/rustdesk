C-Desk-Vint for IRIX
====================

A RustDesk agent for Silicon Graphics machines running IRIX 5.3 or later:
it lets a stock RustDesk client (Windows, macOS, Linux, phones) see and work
the SGI's screen, copy and paste with it, and move files to and from it.

It is built for the MIPS I instruction set, so it runs on every SGI that runs
IRIX 5.3 -- down to an R3000 IRIS Indigo -- and on later IRIX too.


What it needs
-------------

- IRIX 5.3 or later, logged in or at the login screen (the agent shows that
  too, and a remote user can log in through it).
- Any display IRIX drives: 8-bit (Entry, XL) or 24-bit, of any size. The
  agent reads the root window through shared memory, and the windows that
  show through colormaps or depths of their own (IRIS GL RGB windows, 24-bit
  windows on an 8-bit root) through SGI's ReadDisplay. See capture= below.
- The X server's MIT-SHM, XTEST and DEC-XTRAP extensions, which IRIX ships.


Install
-------

With IRIX's own installer (Software Manager, or as root in a shell):

    mkdir /usr/tmp/cdv; cd /usr/tmp/cdv; tar xf C-Desk-Vint-IRIX-5.3+-*.tardist
    inst -f /usr/tmp/cdv        (then: install cdeskvint, go, quit)

or, from the .tar, as root in the unpacked directory:

    ./install.sh

Either copies the agent to /usr/local/sbin, starts it, starts it at every
boot (chkconfig cdeskvint), and prints the machine's RustDesk ID and
password. Print them again at any time with:

    /usr/local/sbin/C-Desk-Vint -i -c /var/C-Desk-Vint/prefs

To remove it: in inst, remove cdeskvint; or ./uninstall.sh (add -a to remove
its settings too). Removing it through inst leaves the running agent to notice
that its program is gone, which takes it up to 10 seconds.


Connecting
----------

- By ID, through an ID server (the default is RustDesk's public one): type
  the ID into the client, then the password. The SGI must be able to reach
  the server: it needs a default route and, for a server given by name, DNS
  (/etc/resolv.conf). Many SGIs on a home network have neither set up.
  If the server's address changes, the agent looks the name up again once
  the server has not answered for a minute and a half.
- Directly by IP address on the same network: in the client, type the SGI's
  IP address as the ID. This needs no server at all.


Settings
--------

As root, Toolchest > C-Desk-Vint opens the settings panel: the ID and
password, the ID server, its key, the relay, a console, how the screen is
read, starting at boot, and Start/Stop. (The Toolchest reads its menu when
it starts: log out and in once after installing for the entry to appear,
or run /usr/local/sbin/cdeskvint-panel.) A change restarts a running agent.

Underneath it is /var/C-Desk-Vint/prefs, one setting a line, which can be
edited by hand; restart the agent after a change:
/etc/init.d/cdeskvint stop; /etc/init.d/cdeskvint start

    password=...    the password (made up on first run)
    server=host     the ID server (hbbs), host or host:port; empty for none
    key=...         that server's public key
    relay=host      the relay (hbbr), if not the one the server names
    port=21118      the port for direct IP connections
    quality=24      VP8 quantiser, 0 (best) to 127; a client can change it
    gamma=1.7       the monitor gamma to undo (default: the system's)
    files=/         what the file manager may see; "off" for no file transfer
    name=...        what peers see this machine called (default: its name)
    api=https://... a RustDesk console (Pro, CortenDesk) to appear in: the
                    agent reports in every 15 s over https. Empty: none.
    capture=auto    how to read the screen: auto (the root by its depth, and
                    windows with colormaps or depths of their own through
                    ReadDisplay); 8 or 24 (the root alone: faster, wrong
                    colours in such windows); readdisplay (everything
                    through ReadDisplay: right everywhere, several times
                    slower).

id, uuid and seed are this machine's identity: keep them, or it becomes a
new machine to every client. The seed is a private key: the file is
readable by root only.


How fast is it
--------------

On a 33 MHz R3000 Indigo: typing and small changes come through in about a
tenth of a second, menus and dialogs in a few tenths. A big change -- a
window opening, the first picture after connecting -- paints in over a few
frames, two to three a second, rather than freezing the picture while it is
encoded. A scrolling terminal is sent as motion (a few kilobytes a frame).
The SGI's own CPU is shared with the X server and the programs, so the
remote picture slows when they are busy.


What it does not do (yet)
-------------------------

- Audio; more than one screen. 24-bit displays are supported but have only
  been tried against a stand-in (see the release notes), not a 24-bit SGI.
- The pointer is shown as a plain arrow on the client, not IRIX's own shapes.
- A client cannot restart or shut down the SGI.
- The console's https handshake takes the R3000 about 7 seconds (it runs in
  a separate, lower-priority process); a server that allows less than that
  -- the one tested allows 10 -- will not list the machine. Certificates are
  checked against ISRG's roots and Let's Encrypt's own intermediates only.
