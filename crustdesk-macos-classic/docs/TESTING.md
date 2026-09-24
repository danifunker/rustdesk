# Testing C-Desk-Vint

Three layers, fastest first.

## 1. The portable core on Linux

```sh
make -C host test
```

- `test_core`: SHA-256 against FIPS vectors, protobuf nesting and zigzag,
  colour conversion at every depth.
- `test_vp8`: 40 frames of a synthetic desktop with the edits a session makes
  (a menu, a caret, a dragged window, scrolling text, the clock), every one
  decoded by the system's libvpx and compared **byte for byte** with the
  encoder's reconstruction. `TEST_W`/`TEST_H` at build time and the quantiser
  as the argument vary it; `SLICE=n` drives the encoder n rows at a time, the
  way the Mac does, and must give identical output.

It needs libvpx's runtime (`libvpx.so.9`) and headers; the headers are taken
from `../rustdesk-ppc-agent/vpx-include`.

`host/build/hostagent [port] [password] [q]` serves a synthetic 8-bit desktop
through the same session and encoder, for trying a real client against the
protocol without an emulator. Mouse clicks leave red marks; keys are printed.

## 2. QEMU's Quadra 800

`scripts/q800.sh` needs a workspace, `$CDV_TESTENV` (default `~/cdv-testenv`):

| file | what it is |
|---|---|
| `f1acad13.rom` | the Quadra 800 ROM (MAME's `macqd800` set) |
| `sys755-net-base.hda` | System 7.5.5 with MacTCP 2.0.6 set to Ethernet, BOOTP |
| `pram-32bit.img` | PRAM with 32-bit addressing on (byte $8A = $65) |
| `qm.py` | a QEMU monitor client: `./qm.py "sendkey d" "shot name"` |

These were made for the DOOM port and copied from `~/doom-mac-testenv/qemu/`;
its RESUME (`DOOM-for-Macintosh/RESUME-retro68-port.md`) says how. The base
disk is never written: every start copies it.

```sh
scripts/q800.sh start          # app in Startup Items, password "classic"
scripts/q800.sh shot NAME      # screenshot -> $CDV_TESTENV/NAME.png
scripts/q800.sh stop
ICOUNT=5 scripts/q800.sh start # paced like a 33 MHz 68040
NOINSTALL=1 DISK2=build-m68k/C-Desk-Vint.hda scripts/q800.sh start
                               # clean system + the release disk, as a user gets it
```

The agent is on `127.0.0.1:31119` (SLIRP forwards it to the Mac's 21118; the
Mac is 10.0.2.15). The agent's log is also written to
`System Folder/Preferences/C-Desk-Vint Log`; after `stop`,

```sh
rb-cli get ~/cdv-testenv/sys.hda@1 "/System Folder/Preferences/C-Desk-Vint Log" log.txt
```

### Timings under ICOUNT

With `-icount shift=5` the guest's clock advances 32 ns per instruction, so
durations the Mac logs are in *its* time, roughly a 33 MHz 68040 (a real one
averages more than one cycle an instruction and multiplies and divides cost
more, so treat them as optimistic). The guest clock runs ahead of wall time --
frames per second counted on the host mean nothing in this mode.

### Mac OS 9 on QEMU's mac99

```sh
scripts/mac99.sh start         # fresh overlay on $OS9_DISK, app on a CD image
scripts/mac99.sh launch        # after ~2 minutes: open the CD and the app
scripts/mac99.sh shot NAME
scripts/mac99.sh stop
```

`$OS9_DISK` defaults to `~/MacOS9-2-2 UTM.qcow2`, a UTM install of 9.2.2; it is
only ever the backing file of a qcow2 overlay made fresh each start, so it is
never written. The application arrives on an HFS CD image because rb-cli
writes HFS, not the HFS+ of the system disk. The first run makes up a
password: read it from a screenshot. The agent is on `127.0.0.1:31129`.

QEMU's mac99 display ignores the gamma table in direct-colour modes, where a
real card applies it; so a decoded peer picture is lighter than QEMU's own.

### Getting files back off a disk image

`rb-cli get` copies only the data fork; `rb-cli get-binhex IMG@N PATH
OUT.hqx` keeps both forks and the type and creator. To make an `.hqx` of a
build: `rb-cli new --fs hfs --size 2M x.hfv`, `rb-cli put-macbinary x.hfv
C-Desk-Vint-fat.bin`, then `get-binhex`.

### When the Mac crashes

`QEMU_EXTRA="-d int -D int.log" scripts/q800.sh start` logs every exception
with its PC (a large file: the timer interrupts are in it too). The last
Address Error or Access Fault, and the A-line traps before it, locate the
crash; `qm.py "xp /8wx ADDR"` reads memory afterwards.

## 3. Driving it

`host/cdvpoke.py HOST:PORT PASSWORD steps...` logs in like a client and plays
input: `move X Y`, `down/up [X Y]`, `rdown/rup`, `key CODE` (Mac virtual
keycode, Map mode), `keydown/keyup CODE`, `type TEXT` (Legacy mode
characters), `sleep S`, `refresh`, `frames S` (count what arrives). For
example, a menu drag:

```sh
host/cdvpoke.py 127.0.0.1:31119 classic move 22 10 sleep 1 down 22 10 \
    sleep 1 move 40 60 sleep 1 up 40 60 sleep 2
```

It also prints any `CursorData`/`CursorPosition` it received and leaves the
last shape's zstd frame in `/tmp/cdv-cursor.zst` (`zstd -d` must accept it).
`CDV_PREFS_EXTRA=$'cursor=separate\r' scripts/q800.sh start` makes the agent
send the pointer separately even though it is in the picture.

`../rustdesk-ppc-agent/target/debug/examples/probe_client HOST:PORT PASSWORD`
is the other agents' protocol probe and works here too.

A real client: `rustdesk --connect 127.0.0.1:31119 --password classic`. On
this machine's X11 session `xdotool` drives it and `xwd -id WINDOW` captures it
(PIL cannot read xwd; a 20-line converter does).
