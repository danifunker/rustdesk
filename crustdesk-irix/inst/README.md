# inst/ -- the inst product description

The inputs to `gendist`(1M) for the `cdeskvint` product, which IRIX's `inst`
and Software Manager install. `scripts/make-inst.sh` (`make inst`) stages the
files beside them, runs gendist on the IRIX machine `scripts/indigo.py`
reaches, and packs the three files it writes (`cdeskvint`, `cdeskvint.idb`,
`cdeskvint.sw`) into a `.tardist`. IRIX 5.3's gendist writes the format every
inst from 5.3 to 6.5 reads; it is in `inst_dev.sw` ("Software Packager") on
the 5.3 development CD (`/CDROM2/dist` on the Indigo), which is not
installed by default.

- **`cdeskvint.spec`**: one product, one image, one subsystem
  (`cdeskvint.sw.base`, default), `replaces self` so installing a later build
  is an upgrade. `@VERSION@` is filled in by make-inst.sh: the build's date
  and the commit's time of day, YYYYMMDDhh with hh in hundredths of a day.
- **`cdeskvint.idb`**: the files, **sorted by destination path** (gendist
  requires it; `LC_ALL=C sort -k5,5 -c inst/cdeskvint.idb` checks). Sources
  are relative to the staging directory: `bin/`, `pkg/`.

What the entries do:

- `etc/config/cdeskvint` is the chkconfig flag, `on`, `config(noupdate)`:
  installed on, and an upgrade keeps it off if someone turned it off.
- `etc/init.d/cdeskvint` with the S99/K01 links starts the agent at boot
  (after xdm, S98) and stops it at shutdown. Its **exitop** runs once at the
  end of an inst session that installed it: stop any running agent, print the
  machine's ID and password (making the identity on first install, before
  the daemon could race to make another), and start it. It does nothing when
  inst installs into another root (`$rbase`, a miniroot install).
- `var/C-Desk-Vint` (0700) holds prefs and log; removing the product leaves
  them, as `uninstall.sh` does without `-a`.
- gendist **strips** the executable (603 KB becomes 462 KB). It runs the same.

**No removeop.** IRIX 5.3's gendist knows `preop`, `postop` and `exitop`
only; a `removeop` is dropped without a word (it came with 6.x). So removing
the product cannot stop the running agent. The agent covers it: started with
`-D` by its full path, it checks every 10 s that the file is still there and
stops when it is gone. An upgrade replaces the file; if the agent stops in
that moment, the exitop starts the new one.

Tested on the Indigo (IRIX 5.3): a fresh install, an install over the tar
install with the agent running (identity kept, no `.O`/`.N` files left), a
removal (the agent stopped by itself within 10 s), and boot start after a
reboot.
