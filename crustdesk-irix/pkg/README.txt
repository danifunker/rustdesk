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
- An 8-bit display (Entry/XL graphics on Indigo, Indy, Indigo2). 24-bit
  screens are not supported yet.
- The X server's MIT-SHM, XTEST and DEC-XTRAP extensions, which IRIX ships.


Install
-------

As root, in the unpacked directory:

    ./install.sh

It copies the agent to /usr/local/sbin, starts it, starts it at every boot
(chkconfig cdeskvint), and prints the machine's RustDesk ID and password.
Print them again at any time with:

    /usr/local/sbin/C-Desk-Vint -i -c /var/C-Desk-Vint/prefs

To remove it: ./uninstall.sh  (add -a to remove its settings too)


Connecting
----------

- By ID, through an ID server (the default is RustDesk's public one): type
  the ID into the client, then the password. The SGI must be able to reach
  the server: it needs a default route and, for a server given by name, DNS
  (/etc/resolv.conf). Many SGIs on a home network have neither set up.
- Directly by IP address on the same network: in the client, type the SGI's
  IP address as the ID. This needs no server at all.


Settings: /var/C-Desk-Vint/prefs
--------------------------------

One setting a line. Restart the agent after a change:
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

- 24-bit displays; audio; more than one screen.
- The pointer is shown as a plain arrow on the client, not IRIX's own shapes.
- A client cannot restart or shut down the SGI.
