# inst/ — the Software Manager product description

These two templates describe the product that IRIX's `inst`(1M) / Software
Manager (swmgr) installs. They are **inputs to `gendist`**, which only exists on
IRIX — so `scripts/iris-gendist.sh` sends them into a running 6.5 guest along
with the staged files and runs the guest's own `gendist` there. Each OS packages
its own build, which is the arrangement `../irixscsitb` uses and the reason the
scripts take an `--abi` rather than assuming n32.

- **`rustdesk-agent.spec`** — product → image → one subsystem, named for the
  ABI. Placeholders are filled at staging time by `stage_inst_inputs` in
  `scripts/ci-lib.sh`: `@VERSION@` (numeric; it begins with `YYYYMMDD`, so
  inst's own numeric comparison orders releases by date), `@SUBSYS@` (`n32`),
  `@ABI_DESC@`. `replaces self` means installing over an existing copy is an
  upgrade rather than a conflict.

- **`rustdesk-agent.idb`** — the file list, **sorted by destination path**,
  which gendist requires and which is easy to break by adding a line at the
  bottom. There is a check:

  ```sh
  LC_ALL=C sort -k5,5 -c inst/rustdesk-agent.idb
  ```

  Source paths are relative to the gendist `-sbase`, which is the staged tree
  `scripts/build.sh` writes: `bin/`, `lib/`, `chest/`.

Two entries are worth explaining:

- **`usr/lib/rustdesk-agent/libgcc_s.so.1`** is a private copy of the GCC
  runtime, and the agent is linked with an rpath naming that directory. It is
  the only library the agent needs that IRIX 6.5 does not ship. Putting it in
  `/usr/lib32` instead would work and would also be a GCC runtime sitting in a
  system directory where some unrelated program finds it in a year's time.

- **`usr/lib/X11/app-chests/RustDesk.chest`** is the Toolchest entry, a drop-in
  fragment pulled in by the `sinclude` at the end of
  `/usr/lib/X11/system.chestrc`. No system file is edited, and its
  `f.checkexec.sh` verb hides the menu item whenever the program is absent — so
  removal needs no hook.

The desktop *icon* — FTR rules, a vector `.icon` file and a type-database
rebuild — is deliberately not here. See `../irixscsitb/desktop/` for what that
involves; it edits the filetype Makefile and runs make, which a package should
not do silently.
