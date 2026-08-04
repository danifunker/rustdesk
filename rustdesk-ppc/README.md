# rustdesk-ppc — the PowerPC agent build

Build tooling for running `rustdesk --server` on a PowerPC Mac (G4/G5, Mac OS X
10.4/10.5). The *why*, the feasibility analysis and the mrustc gap list live in
[`../docs/powerpc-mrustc-scope.md`](../docs/powerpc-mrustc-scope.md); this file
is the *how*.

Branch `ppc-agent`, based on upstream **`1931cb8c7`** (v1.1.8, 2022-01-05) — the
last era with a GUI-free `cli` feature *and* `direct_server` (direct-IP access,
so no ID server is strictly required).

## Goal

The G5 is the **remote guest**: you connect *into* it from a modern client. Only
the `--server` role is in scope. No viewer, no address book, no sciter.

## Layout

| path | what |
|---|---|
| `build-ppc.sh` | the driver — `vendor`, `hbb`, `agent`, `clean` |
| `patches/git-deps.py` | rewrites `git =` deps to `path =` into `vendor/` (minicargo cannot read git deps); reversible |
| `stub-cc.sh` | fake target C compiler, for exercising the Rust front-end with no PowerPC Mac |
| `vendor/` | `cargo vendor` output (gitignored, ~444 crates) |

## The two-machine model

mrustc is a transpiler — Rust in, C99 out. It never emits a PowerPC binary; a
PowerPC-Darwin C compiler does, and there is no sane cross-gcc for it, so the C
is compiled *on the Mac*:

```
   This machine (fast)                PowerPC Mac (over ssh)
   -------------------                ----------------------
   Rust --mrustc--> C99        --->   C99 --gcc10--> PowerPC Mach-O
```

`scripts/ppc-cc-remote.py` from the **rusty-backup** tree *is* the C compiler as
far as mrustc is concerned. Point `CC_powerpc_apple_darwin` at it (with
`PPC_HOST` set) and every codegen step ships its `.c` over ssh, runs gcc there,
and copies the `.o` back — minicargo's dependency graph, parallelism and
incremental rebuilds keep working across the two machines.

## Running it

```bash
# 1. re-resolve + re-vendor (only needed when dependencies change)
rustdesk-ppc/build-ppc.sh vendor

# 2. front-end only, no PowerPC Mac needed -- the fastest error signal
PPC_STUB_CC=1 rustdesk-ppc/build-ppc.sh hbb      # protocol core
PPC_STUB_CC=1 rustdesk-ppc/build-ppc.sh agent    # the whole agent

# 3. the real thing, once a G5 is reachable
export PPC_HOST=admin@g5.local
export CC_powerpc_apple_darwin=~/repos/rusty-backup/scripts/ppc-cc-remote.py
export AR_powerpc_apple_darwin=~/repos/rusty-backup/scripts/ppc-ar-remote.py
rustdesk-ppc/build-ppc.sh agent
```

`PPC_STUB_CC=1` swaps the C compiler for a stub that emits an empty object.
mrustc errors still surface; only codegen is fake. **Objects built that way are
junk** — it is a front-end test, nothing more.

Requires a PowerPC stdlib at
`$MRUSTC_DIR/output-1.74.0-powerpc-apple-darwin-<cpu>` (already built for
g3/g4/g5 from the rusty-backup work) and the mrustc fork with the async fixes
listed in the scope doc.

## Setting it up on the Mac (no GUI to do it from)

```bash
rustdesk --password 'something-long'          # REQUIRED, see below
rustdesk --rendezvous-server hbbs.example.com # self-hosted ID server
rustdesk --key '<hbbs public key>'
rustdesk --get-id                             # the ID to connect to
rustdesk --server                             # run the agent
```

## Security: the permanent password is not optional

`src/cm_headless.rs` replaces upstream's sciter connection-manager window. When
a peer connects *without* a valid password, upstream shows an accept/reject
prompt; headless, there is nobody to click it, so this build replies
`Data::Authorize` — **it accepts everyone**.

The permanent password is therefore the only thing between "unattended" and
"unauthenticated". Both `--server` and `--cm` refuse to start without one. Do
not remove that check; on a machine with no screen a warning would go unread.

(With a *correct* password, the CM is not in the auth path at all —
`Connection::on_message` calls `send_logon_response()` and only then
`try_start_cm(.., authorized = true)`, i.e. the CM is told, not asked.)
