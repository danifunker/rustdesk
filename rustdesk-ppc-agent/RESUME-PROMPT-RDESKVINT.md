# DONE: R-DeskVint 1.0.0 (PowerPC) built and shipped — 2026-09-03

Both CPUs linked, the universal artifacts are built and verified, and they are
attached to the **draft** release `r-deskway-1.5.0` on `danifunker/rustdesk`
(still a draft — nothing is public until someone publishes it).

Artifacts, in
`/home/dani/repos/rustdesk/rustdesk-ppc-agent/target/release/1.0.0/`:
`R-DeskVint-PPC-1.0.0.dmg`, `rustdesk-agent-1.0.0.tar.gz`, `SHA256SUMS`,
`MANIFEST.txt`. Stamped `1.0.0 (d59dd74e4)`.

Verified: universal fat binary with real PowerPC/7450 and PowerPC/970 slices; no
64-bit instructions in the G4 slice; bundle is `R-DeskVint.app` with
`CFBundleName`/`CFBundleDisplayName` = R-DeskVint while `CFBundleExecutable`,
`com.rustdesk.ppc-agent` and the config paths are unchanged (the drop-in property
holds); icon is rust-orange. Not verified: no G4 hardware was available.

## The blocker, and why the old prompt's diagnosis was wrong

This file used to say the isolated mrustc (`a54c2e38`) transpiled protobuf big
enough to split, so two crates split where only one had before. **That was
false.** `libprotobuf-3_0_0.rlib.c` is byte-identical between the 08-18 tree and
this one (84,446,548 bytes, same md5), and both crates were already split in the
build that shipped.

The real cause: `ppc-split-tu.py` renamed promoted crate-local symbols with a
*constant* `__rbsplit` suffix, which separates a promoted copy from the crate
that owns the symbol but not two split crates from each other. The 08-18 build
linked only because protobuf kept salted artifacts from an uncommitted 08-07
build of the splitter while the agent was split by the reverted one. Luck, not
design — any clean rebuild of that tree would have failed identically.

Fixed by making the suffix per translation unit
(`__rbsplit_<sha256(basename)[:8]>`), with the suffix in `.split/.stamp` so a
scheme change invalidates old splits. The change lives **uncommitted** in
`~/repos/rusty-backup` (`scripts/ppc-split-tu.py`, `docs/build-ppc-mrustc.md`);
the full write-up is `~/repos/rusty-backup/docs/RESUME-ppc-split-rename-suffix.md`.

The trap worth remembering if you touch the splitter again: **minicargo's
staleness check is the 0-byte `.rlib` metadata stub, not `.rlib.o`.** Deleting
just the objects makes it report the crate cached and link against files that no
longer exist. Remove the `.rlib` to force a re-split.

## If you need to rebuild

```bash
cd /home/dani/repos/rustdesk/rustdesk-ppc-agent
export MRUSTC_DIR=/home/dani/repos/mrustc-ppc
export SSH_AUTH_SOCK=/run/user/1000/keyring/ssh
export MBEDTLS_INCLUDE_DIR_powerpc_apple_darwin=/Users/admin/ppc-libs/include
export MBEDTLS_LIB_DIR_powerpc_apple_darwin=/Users/admin/ppc-libs/lib
./build-release.sh -H ppctiger        # ~25 min with the toolchain cached
```

Do not touch the SPARC tree (`~/repos/mrustc` on `sparc-solaris-10`) or rebuild
the isolated PowerPC toolchain; both are fine. `R-DeskVint` still covers the
SPARC and IRIX vintage agents eventually — this was PowerPC only.
