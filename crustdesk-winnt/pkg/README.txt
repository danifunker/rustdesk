C-Desk-Vint for Windows NT
==========================

A RustDesk agent for Windows NT 4.0: it lets a stock RustDesk client
(Windows, macOS, Linux, phones) see and work this machine's screen, copy and
paste with it, and move files to and from it -- across the room by its
address, or across the internet by its RustDesk ID.

This build is for one processor; the file name says which (PPC: PowerPC,
x86: Intel, Alpha: DEC Alpha, ARM: PINT, Windows NT 4.0 on the Raspberry
Pi 2, 3 and 4 -- github.com/tenox7/PINT). The same program is built for each from the same
source.

The x86 build runs on Windows 98 (and should on 95) too: there it is a
program started at boot rather than a service, it shows the logon prompt
and keeps running across logoffs, and its file names and clipboard use the
machine's code page (a character the code page lacks becomes "?"). There is
no Ctrl+Alt+Del to send on 9x. Tested on Windows 98 SE; not on 95.


What it needs
-------------

- Windows NT 4.0 (Workstation or Server), or on x86 Windows 98, or PINT
  on a Raspberry Pi, with TCP/IP.
- An Administrator to install it.
- Any display NT drives, at any depth: 256 colours, 16-bit or 24-bit.


Install
-------

Copy C-Desk-Vint.exe to the machine (a floppy, a network share, FTP) and
run it. The settings window opens; press Install. It copies itself to
Program Files\C-Desk-Vint, installs the "C-Desk-Vint remote desktop"
service (it starts with Windows, before anyone logs on), starts it, puts
C-Desk-Vint in everyone's Start Menu, and opens the installed copy's
settings window. It also starts the C-Desk-Vint tray (below), and sets
it to start whenever someone logs on.

The window shows this machine's ID and password: that is what a RustDesk
client connects with. Once the status line says "Ready: registered with
rs-ny.rustdesk.com", the machine can be reached by its ID from anywhere.


The settings window
-------------------

Start Menu > Programs > C-Desk-Vint. Change a setting and press Apply: the
agent restarts with it.

  Password        what a client must give. "New" makes a random one.
  Service         install, start, stop or remove the agent service.
  ID server       the RustDesk ID server (hbbs), host[:port]. The default
                  is RustDesk's public server; give your own if you run one.
                  Empty: no ID, reachable by address only.
  Key             that server's public key (its id_ed25519.pub).
  Relay server    the relay (hbbr), if not the one the ID server names.
  Console (API)   a RustDesk Pro or similar console's address, to appear in
                  its device list (https://host[:port]). Empty: none.
  Direct IP port  where a client can connect by this machine's address
                  (21118, as RustDesk's own). 0 turns it off.
  Answer LAN discovery  lets clients on this network list the machine.
  File access     what the client's file manager may see: / for all
                  drives, files and folders; off for none; or one folder
                  (D:\Shared) for just that -- only one.
  Picture quality 0 (best) to 127 (smallest); 24 is the default. A client
                  can change it during a session.
  Read only what changed (screen hooks)
                  on by default: during a session, cdvhook.dll (beside the
                  program) is loaded into the programs on the desktop and
                  tells the agent where they draw, so it reads that and not
                  the whole screen -- much less work on a slow machine.
                  Turn it off if a program misbehaves with it; the agent
                  then reads the whole screen, as before.
  Log folder      where C-Desk-Vint.log goes (D:\Logs); empty: beside the
                  program. Note that anyone who can read that folder can
                  read the log: who connected, from where.

"View log" opens C-Desk-Vint.log, which says what the agent is doing.


What a client can do
--------------------

- See the screen and use the mouse and keyboard, including at the logon and
  locked screens: the client's "Insert Ctrl+Alt+Del" brings up the Windows
  NT Security dialog, as the keys would at the machine.
- Copy and paste text both ways (any language).
- Open the file manager beside the desktop: list, download, upload, make
  folders, rename and delete.
- Take a screenshot, restart the machine.

- Chat with whoever is at the machine (the tray, below, shows it).
- On Windows 2000 and later: "Insert lock" locks the screen, and "Block
  user input" stops the machine's own keyboard and mouse until it is
  turned off (or Ctrl+Alt+Del is pressed there). Windows NT 4.0 cannot do
  either; 98 can block input. Neither has been tried on Windows 2000 yet.

One desktop session at a time, with a file manager beside it.


The tray
--------

On Windows NT, an icon by the clock for whoever is logged on (it starts at
logon; C-Desk-Vint -tray starts it by hand). Its tip says who is connected.
Right-click it for:

  Chat...                     the chat window. It opens by itself when the
                              person connected writes; type below and press
                              Enter to answer.
  Disconnect remote sessions  end every remote session now.
  Settings...                 the settings window.
  Exit tray                   until the next logon.

Without the tray nobody can read chat here; a client that writes is told
so. Only the user logged on at the machine's own screen may use the tray.
Not on Windows 98 yet.


From the command line
---------------------

  C-Desk-Vint -i          print the ID and password
  C-Desk-Vint -install    install and start the service (from where it is)
  C-Desk-Vint -tray       the tray (normally started at logon)
  C-Desk-Vint -hook off   read the whole screen, without the screen hooks
                          (-hook on to use them again); restarts the agent
  C-Desk-Vint -logdir D:\Logs
                          write the log there (-logdir alone: beside the
                          program); restarts the agent
  C-Desk-Vint -remove     stop and remove the service
  C-Desk-Vint -start      start the service; -stop to stop it
  C-Desk-Vint -run [-v]   run the agent in a console window instead (testing)
  C-Desk-Vint -selftest   check that this build computes right on this machine

Everything lives beside the program: C-Desk-Vint.cfg (the settings),
C-Desk-Vint.log, C-Desk-Vint.sta (what the settings window shows).


Security
--------

- A connection through the ID server is encrypted end to end, as RustDesk's
  own are. A connection to the direct IP port is not -- that is how
  RustDesk's direct access works too. On a machine the internet can reach,
  set the port to 0, or keep it behind a firewall.
- C-Desk-Vint.cfg holds the password and the machine's private key. On an
  NTFS disk only Administrators and SYSTEM can read it. A FAT disk keeps no
  permissions: there, anyone who can log on to the machine can read it.
- Choose a password of your own (6 or more characters) if the machine is
  reachable from the internet.


Remove
------

Settings window > Remove (or C-Desk-Vint -remove), then delete
Program Files\C-Desk-Vint.


C-Desk-Vint is part of a RustDesk fork for vintage machines, under the
AGPL-3.0. It contains BearSSL (MIT), parts of libsodium (ISC) and zstd's
decoder (BSD).
