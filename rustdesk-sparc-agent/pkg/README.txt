R-DeskVint -- remote desktop agent for SPARC Solaris
====================================================

The controlled side of a remote desktop session, speaking the RustDesk
protocol. A peer connects to this machine and sees its console.

Built for 64-bit SPARC against Solaris 9, so it runs on Solaris 9 and later.


INSTALLING
----------

    pkgadd -d RDVTagent-<version>-sparc.pkg RDVTagent

Everything lands under /opt/rdeskvint. Installing changes nothing about how the
machine starts up: no daemon, no rc script, nothing disabled. Wiring it into
startup is a separate, reversible step -- see AUTOSTART below.

    /opt/rdeskvint/bin/rdeskvint          the agent
    /opt/rdeskvint/bin/rdeskvint-enable        wire it into startup
    /opt/rdeskvint/bin/rdeskvint-disable       take it back out
    /opt/rdeskvint/bin/rdeskvint-console-test  prove it end to end
    /opt/rdeskvint/lib/                        GCC runtime, startup templates
    /opt/rdeskvint/doc/README.txt              this file

Two symlinks go on the default PATH, so the commands you type are just there:

    /usr/bin/rdeskvint            -> /opt/rdeskvint/bin/rdeskvint
    /usr/bin/rdeskvint-session    -> /opt/rdeskvint/bin/rdeskvint-session

Nothing edits /etc/profile or any other shared file. The admin commands
(rdeskvint-enable, -disable, -share-config, -console-test) are not linked; run
them from /opt/rdeskvint/bin, as the examples here do.

Removing:

    pkgrm RDVTagent

which un-wires startup first, so removal cannot leave the machine with an rc
link pointing at a deleted directory or -- worse -- with no login manager.

Your configuration is NOT removed. It lives in ~/.rdeskvint.conf and holds this
machine's identity; delete it by hand if you want that gone too.

An agent from before this file was renamed kept its settings in
~/.rustdesk-ppc-agent.conf. That one is still read when ~/.rdeskvint.conf does
not exist, so an existing machine keeps its ID, its uuid and its keypair rather
than quietly becoming a new machine -- which matters more than it sounds, since
hbbs pins the first uuid it sees for an ID and refuses every later one. The
first setting you change writes the new file; the old one is left alone rather
than deleted, and the agent says which it read.


CONFIGURING
-----------

Settings live in ~/.rdeskvint.conf, one `key = value` per line, mode 0600
because the machine's signing key is in it. It is created on first use and you
should not need to edit it -- every setting below is written there by the binary
as soon as you set it, and applies on every start from then on:

    rdeskvint --password SECRET    set the password (6 characters or more)
    rdeskvint --show-id            the ID a peer connects to
    rdeskvint --show-key           the public key a peer can pin

That is a complete direct-IP setup. Run it, connect a peer to this machine's
address on port 21118, give the password.

Optional, persisted, and independent of one another:

    rdeskvint --id-server HOST        register with a rendezvous server, so
                                        the machine is reachable by ID from
                                        anywhere rather than by IP on this LAN
    rdeskvint --key KEY            the server's key; only needed if the
                                        relay was started with -k
    rdeskvint --api-server URL     report in to a console, so the machine
                                        appears in its device list
    rdeskvint --ca-bundle PATH     certificates, for a console behind a
                                        private CA

--id-server makes the machine REACHABLE. --api-server makes it VISIBLE. Neither
implies the other. Each has a --no-... form that switches it back off.

    rdeskvint --scale 1                 serve the screen at full size
    rdeskvint --scale 2                 half in each direction
    rdeskvint --scale 0                 back to this platform's default

Scale is a resolution dial, in powers of two up to 8. The Solaris default is 2,
inherited from the IRIX port where it was measured on a far slower machine, and
on a Blade 2500 full size is perfectly usable. Measured here at 1280x1024 with a
terminal scrolling continuously -- so, the whole screen changing every frame:

    scale 1   2.25 fps    74 KB/frame
    scale 2   5.04 fps   8.5 KB/frame

That is the floor, not the typical case: frames are sent when something changes,
so an ordinary desktop where one window moves costs a fraction of it. There is
no DAMAGE on this machine, so every frame is a full-screen read either way --
what scale changes is the conversion and the encode, both of which cost per
pixel.

A peer that asks for a particular image quality overrides this for its own
session: a client set to Best asks for full size whatever the setting says.

    rdeskvint --listen ADDR --port N     default 0.0.0.0:21118
    rdeskvint --log debug                or --log trace, to watch a
                                              handshake message by message

--secure requires a signed-identity exchange and is OFF by default. Leave it
off for direct-IP connections: a client connecting by address does not take
part in that exchange, and turning it on deadlocks the handshake.


POINTING IT AT A CORTENDESK SERVER
----------------------------------

The four settings are the four fields a RustDesk client shows, with the same
names and the same values. Whatever you put in the client, put here:

    client field      flag                     config file key
    ------------      ----                     ---------------
    ID Server         --id-server HOST         id_server
    Relay Server      --relay-server HOST      relay_server
    API Server        --api-server URL         api_server
    Key               --key BASE64             key

Two of them do two different jobs. You almost certainly want both.

    rdeskvint --id-server hbbs.example.org
    rdeskvint --api-server https://console.example.org

--id-server registers with hbbs and makes the machine REACHABLE by ID from
anywhere, rather than by IP on this LAN. HOST or HOST:PORT; the default port is
21116.

--api-server points at the console's HTTP API and makes the machine VISIBLE in
its device list. A bare host means https -- assuming http would silently
downgrade a console reachable over TLS and say nothing. CortenDesk's container
port is 8080, so a local deployment is usually:

    rdeskvint --api-server http://192.168.1.10:8080

Neither implies the other, and this is the trap worth knowing: the console
builds its device list from that HTTP API and NOT from hbbs registration. An
agent given only --id-server registers perfectly, is connectable by ID, and appears
in no list at all -- which from the console looks exactly like an agent that
does not work.

Both settings are saved, so they apply on every start from then on.
--no-id-server and --no-api-server switch them back off, separately.

    rdeskvint --relay-server HOST
    rdeskvint --key '<base64>'

Whether you need these two depends on the deployment, and the honest answer is
to configure the agent the same way the clients on that deployment are
configured. If your clients need all four fields filled in to work over the
internet, so does this -- it is the same protocol and the same server.

Relay Server empty means "whichever relay the ID server advertises", which is
right when hbbs advertises a publicly reachable one (its -r). Set it when it
does not: the failure is a call that connects and then dies with no video, and
the agent's log names the relay it was told to use --

    rendezvous: relay request from <peer> via <relay>, uuid ...

-- so a private address there is the diagnosis.

Key goes into RequestRelay.licence_key and is checked only by a RELAY started
with -k. A keyed relay drops a request whose key does not match by simply
returning, so the symptom is a caller waiting forever on a relay this agent
appears never to have joined, with nothing logged at either end. An unkeyed hbbr
ignores it, so setting it when it is not needed costs nothing -- which is a good
reason to just set it.

Note hbbs is keyed even without -k, because it generates id_ed25519 for itself,
while hbbr has no such fallback. The two are configured separately, and that is
the usual source of confusion here.

The CLIENT needs that same key -- the server's id_ed25519.pub -- in
Settings -> Network -> ID/Relay Server, exactly as for any other peer. The ID
from `rdeskvint --show-id` goes in the ID field.

    rdeskvint --ca-bundle /path/to/ca.pem

only if the console is behind a PRIVATE CA. For a public certificate you need
nothing: the package ships /opt/rdeskvint/bin/cacert.pem, which is the first
place the agent looks. Solaris 9's own trust store is from 2002 and cannot
verify anything on today's internet, so an https API Server would otherwise fail
at the handshake with "no CA bundle found".

Two things about sessions through a server:

  * They are ALWAYS encrypted, whatever --secure says. A peer arriving via the
    server takes part in the signed_id/public_key exchange; --secure only ever
    concerned the direct-IP listener, where nobody does.

  * A caller on the same subnet is handed our address and connects directly;
    anyone else meets us at the relay. There is no hole punching.

LAN discovery answers broadcasts on udp/21119, so the machine turns up in a
client's local-network list by hostname without any of the above.


ONE MACHINE, WHOEVER LOGS IN
----------------------------

The agent runs inside whoever's session is on the console, and by default reads
~/.rdeskvint.conf from that account's home. So a second account that logs in is
a SECOND MACHINE as far as the server is concerned: another ID, another row in
the device list, another uuid -- and hbbs pins the first uuid it sees for an ID
and refuses every later one, for ever. Logging in as root when everything was
set up as someone else is the usual way to meet this; the agent starts, finds no
password, and exits.

    rdeskvint-share-config -u USER

copies that account's configuration to /etc/opt/rdeskvint/agent.conf, which the
agent prefers over any per-user file. Every session then runs as the same
machine, root included. `rdeskvint-share-config -r` undoes it.

The cost is that the shared file has to be readable by every account that logs
in, and it holds the connection password and this machine's private key. It is
installed root-owned, mode 0640, group-readable by the account's own group
unless -g names another. Anyone in that group can impersonate this machine.
Decide before running it.

The agent prints which file it used, so there is never a question:

    config    : /etc/opt/rdeskvint/agent.conf


STARTING IT BY HAND
-------------------

    rdeskvint-session start | stop | status | log

Run it as yourself, from a terminal inside the desktop session you want served.
The autostart hook does exactly this at login; this is the same thing on demand,
for a session that was already running when the agent was installed, or after
stopping it to change a setting.

It needs that session's DISPLAY and X authority, which is exactly what an ssh
connection does not have -- so this is one of the few things here that cannot be
done remotely.


AUTOSTART
---------

    rdeskvint-enable                    console session mode (the default)
    rdeskvint-enable -m boot -u USER    standalone mode
    rdeskvint-disable                   undo either, and stop a running agent

Both are safe to re-run, and disable does not need to be told which mode was
used.

SESSION MODE is the default and almost certainly the one you want. It drops a
hook in /etc/dt/config/Xsession.d. The agent starts when somebody logs in on the
console, runs as that person, sees exactly what they see, and exits with their
session. CDE is untouched and the machine still shows its normal login screen.

    The catch is in the name: no session, no agent. The machine is reachable
    only while somebody is logged in at the console.

BOOT MODE makes the machine reachable with nobody logged in, and it costs you
CDE. It turns the CDE login manager off (dtconfig -d) and installs
/etc/init.d/rdeskvint plus an rc link, which brings up its own Xsun on the
framebuffer -- with dtwm and a terminal, so there is something to connect to --
and runs the agent against it as USER.

    /etc/init.d/rdeskvint start | stop | status | restart

    This is not a matter of taste. CDE's greeter holds an X server grab that
    hangs every X client inside XOpenDisplay, with no error and no timeout. An
    agent that runs with nobody logged in therefore cannot share the console
    with dtlogin. It is one or the other.

rdeskvint-disable turns dtlogin back on and starts it, so you get your login
screen back without a reboot.


THE DISPLAY, WHICH IS WHERE THE SURPRISES ARE
---------------------------------------------

The agent captures a real X display, and on this hardware that is the console.
Two things will stop it dead, both silently:

  * The CDE greeter's server grab, described above. A client blocks INSIDE
    XOpenDisplay -- no error, no timeout, just silence. If the agent appears to
    hang at startup and nobody is logged in, this is why.

  * There is no virtual framebuffer to retreat to on Solaris 9.
    /usr/openwin/bin/Xvfb is a wrapper for `Xsun -dev vfb` and the vfb module is
    not installed.

Two things about the framebuffer itself, on an XVR-600:

  * Every TrueColor visual it offers is BGR -- red in the low byte. The agent
    detects this and swaps the channels as it captures. If you are testing a
    colour problem, use coloured content: black-and-white cannot show a red/blue
    swap, being symmetric under it.

  * Xsun advertises no DAMAGE and no XFIXES. So every frame is a full-screen
    read rather than a list of changed rectangles, and the remote cursor is
    drawn rather than read from the server. The agent says so at startup:

        capture path: MIT-SHM, full screen per poll  -- NO DAMAGE TRACKING
        cursor: XFIXES absent; falling back to a drawn arrow

    Both lines are expected on this machine and are not faults.


CHECKING IT
-----------

    rdeskvint --probe-display

reports what the framebuffer looks like and times the capture, conversion and
encode path with no networking involved. It runs a full VP8 tuning sweep, so
give it a few minutes.

    rdeskvint-console-test SECRET

is the whole thing end to end: it stops the greeter, brings up Xsun on the
framebuffer, starts the agent, points a real protocol client at it that decodes
the video that comes back, and hands the console back to CDE afterwards. It has
to run as one command over ssh, because Xsun dies with the session that started
it. A good run ends:

    VERDICT: the agent is serving video to a peer.


WHAT THIS IS
------------

An unofficial agent that speaks the RustDesk protocol. It is not produced by or
affiliated with the RustDesk project. It reports its platform to peers as
"Linux", on purpose: clients match that string against a known set to decide how
to translate keystrokes, and an X11 desktop with Control-based shortcuts is what
the Linux entry describes. A truthful "Solaris" would fall through to whatever
each client's default happens to be -- in some, the Windows icon and Windows key
handling.
