# RustDesk on PowerPC Mac OS X (G4/G5, 10.4/10.5) — feasibility scope

Scoping study, 2026-08-04. Target: a usable RustDesk on a G5 running Leopard
10.5, reusing the mrustc/PowerPC toolchain already proven by `rusty-backup`.

**Verdict: the toolchain is not the problem. `async` is.** Everything measured
below was run, not assumed; each claim says how it was checked.

---

## 1. What already exists (and is reusable as-is)

`rusty-backup` shipped a real PowerPC binary through this pipeline. Verified:

```
$ file ~/repos/mrustc/output-rb-ppc-g5/rb-cli
Mach-O ppc_970 executable, flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|...>   (100 MB)
```

Also present: `output-rb-ppc-g3`, `-g4`, and a complete PowerPC standard library
(`output-1.74.0-powerpc-apple-darwin-g5/`: core, alloc, std, panic_unwind, test,
libc — 20 rlibs). The `danifunker/mrustc` fork carries a merged
`powerpc-apple-darwin` target plus ~24 fixes.

The infrastructure that carries over unchanged:

| Piece | What it does |
|---|---|
| Two-machine model | mrustc emits C99 here; `gcc-10.5` on the Mac emits PowerPC Mach-O |
| `scripts/ppc-cc-remote.py` | *is* `CC_powerpc_apple_darwin` — ships `.c` over ssh, returns `.o` |
| `scripts/ppc-ar-remote.py` | `AR_powerpc_apple_darwin` (Apple `ld` needs a Mach-O `__.SYMDEF`) |
| PowerPC libstd | already built for G3/G4/G5 |
| Link-line knowledge | `-latomic`, `-lMacportsLegacySupport`, `-lgcc_s.1`, `ppc-compat.o` |
| TU splitting | `ppc-split-tu.py`, for the 16 MB branch-displacement limit |

Scale is comparable: `rusty-backup`'s PPC build resolves ~380–404 crates.
RustDesk's macOS graph is **376 crates** (`cargo tree --target x86_64-apple-darwin`),
so nothing here is out of family on size alone.

---

## 2. The blocker: mrustc and `async`

RustDesk is tokio from top to bottom — **395 `async fn`**, **46 `async {}`
blocks**, **73 `tokio::spawn`/`select!` sites** across `src/` + `libs/vintage_common`.
So mrustc's async support is decisive. I measured it directly with a hand-rolled
executor against the 1.74 host stdlib (`MRUSTC_TARGET_VER=1.74 --edition 2021`):

| Pattern | Status |
|---|---|
| `async fn` + `.await` chains | ✅ compiles and runs correctly |
| bare `async { }` polled/awaited at top level | ✅ **fixed here** (§2a) |
| `pub async fn` inside a `macro_rules!` `:item` capture | ✅ **fixed here** (§2e) |
| `async fn` nested inside a function body | ✅ **fixed here** (§2e) |
| `async { }` nested inside an `async fn` | ❌ MIR lowering bug |
| `Pin<Box<dyn Future>>` (what `#[async_trait]` emits) | ❌ no vtable for `Future::poll` |

### 2a. Fixed during this scoping: `Future` for async blocks was gated at 1.90

mrustc *has* the `Future` impl for anonymous async-block types, in
`StaticTraitResolve::find_impl` — but it was gated behind `TARGETVER_LEAST_1_90`,
while the inference side (`hir_typeck/helpers.cpp:1719`) already allows it at
1.74. That inconsistency made every `async {}` block abort the compiler:

```
BUG: ASSERT FAIL: src/hir_typeck/static.cpp:2136:valid_for_opaque(input):
  Set opaque on a non-generic type: <async[0x…] as core::future::future::Future>::Output
```

One-token change (`src/hir_typeck/static.cpp:426`), 20-second rebuild:

```diff
 TU_ARMA(Async, node_p) {
-    if( TARGETVER_LEAST_1_90 && trait_path == m_lang_Future )
+    if( TARGETVER_LEAST_1_74 && trait_path == m_lang_Future )
```

Bare async blocks now compile **and run correctly**. Left on branch
`async-block-future-1.74` in `~/repos/mrustc`, uncommitted.

### 2b. Still broken — the two that matter

**Nested async block inside an `async fn`.** The outer `async fn` is rewritten to
a generator struct, but the inner block's type isn't rewritten with it:

```
MIR ERROR: <bin::future#outer_5 as core::future::future::Future>::poll BB10/0: Type mismatch:
 dst : &'_ mut bin::future#outer_2
 src : &'_ mut async[0x…]
```

This is `tokio::spawn(async move { … })` called from inside an `async fn` — the
single most common shape in the codebase. Fix lives in `hir_expand/closures.cpp`
(expansion ordering); depth not established.

**`Pin<Box<dyn Future>>`.** Dynamic dispatch on `Future` is unimplemented:

```
TODO: Trans_AutoImpls - Handle different receiver types: <dyn Future>::poll
  - self: Pin<&'M0 mut dyn Future<Output=u32>>
```

`trans/auto_impls.cpp:522` handles `self`, `&self`, `&mut self` and `Box<self>`
but not the `Pin<&mut Self>` arbitrary receiver. Bounded work (Pin is
`repr(transparent)` over the pointer, so it should reduce to the borrow case),
but real. RustDesk's own `#[async_trait]` use is only **4 sites** and is
hand-rewritable; `futures-util`'s `BoxFuture`, tokio's `LocalSet` and
hyper/tower internals are not.

### 2c. libc — looked like a blocker, is actually a version pin

libc's `src/new/` module tree defeats mrustc's name resolution on Apple targets
— three distinct failures, each surfacing after the previous was patched out:

```
new/mod.rs:204  Cannot find component 1 of crate::new::pthread_::spawn
new/mod.rs:209  Cannot find component 1 of crate::new::sys::ttycom
new/common/posix/pthread.rs:28  Couldn't find type name 'pthread_attr_t'
```

`rusty-backup` sidestepped this by pinning `libc = 0.2.155` (pre-`src/new/`),
and the first read of this was that RustDesk *couldn't*, because `tokio 1.44.2`
requires `libc ^0.2.168`. That was wrong. Bisecting when `src/new/` actually
landed:

| libc | `src/new/`? |
|---|---|
| 0.2.168 – 0.2.174 | **no** |
| 0.2.175 + | yes |

So `libc = "=0.2.174"` satisfies tokio's `^0.2.168` *and* predates the
restructure. **Verified: libc 0.2.174 transpiles clean for
`powerpc-apple-darwin`.** No RustDesk downgrade needed, no mrustc fix needed —
one pin in the PPC manifest, exactly the `rb-cli-ppc` deviation pattern.

### 2d. How far tokio actually gets

With that pin, a minimal tokio build (`rt`, `rt-multi-thread`, `net`, `io-util`,
`macros`, `time`, `sync`) for `powerpc-apple-darwin` gets **the entire
dependency tree through mrustc**:

```
Completed  libc 0.2.174   bytes 1.10.1   mio 1.1.0      socket2 0.5.10
           pin-project-lite 0.2.17       syn 2.0.119    quote 1.0.47
           proc-macro2 1.0.107           tokio-macros 2.5.0   unicode-ident 1.0.24
```

tokio *itself* then hits a succession of small, loud parser gaps — two of which
I fixed in the course of this scoping (§2e). It is currently stopped at a third:

```
MACRO<tokio::cfg_unstable_metrics> BUG: src/parse/root.cpp:1327:
  TODO: Parse_Impl_Item - Interpolated item into impl: MacroInv
```

(The same `Parse_Impl_Item` interpolated-item path `docs/build-ppc-mrustc.md`
records two earlier fixes in.)

To exercise the Rust front-end without a reachable PowerPC Mac, point
`CC_powerpc_apple_darwin` at a stub that emits an empty object — mrustc errors
still surface, only the C compile is skipped.

### 2e. Two more fixes landed during this scoping

**`$item:item` could not match `pub async fn`** — `consume_item` in
`src/macro_rules/eval.cpp` consumed `unsafe` but not `async`, so tokio's
`cfg_io_util! { pub async fn copy…() }` aborted the compiler. 8-line repro.

```diff
-        if(lex.next() == TOK_RWORD_UNSAFE)
-            lex.consume();
+        while( lex.next() == TOK_RWORD_ASYNC || lex.next() == TOK_RWORD_UNSAFE )
+            lex.consume();
```

**`async fn` as a block-level item** — `src/parse/expr.cpp` gave `const` and
`unsafe` an "is the next token `{`? then it's an item, not an expression"
check, but not `async`, so a nested `async fn` inside a function body parsed as
an `async` block and died on `fn` (`Unexpected token TOK_RWORD_FN, expected
TOK_BRACE_OPEN` — tokio `process/mod.rs:1360`). Added the matching case.

Both verified against minimal repros and the full async test set.

---

## 3. Blockers that have nothing to do with mrustc

Even with a perfect compiler, these stand between here and a running app:

**The GUI does not exist for this platform.** RustDesk offers two front-ends and
neither can work:
- *sciter* — a closed-source prebuilt binary, shipped for x86/x86_64/aarch64
  only. There is no PowerPC build and there cannot be one.
- *Flutter* — no PowerPC Darwin engine.

Anything with a window must be hand-written Carbon/Cocoa, exactly as
`rusty-backup` did in `ppc-tiger/` (`rusty_backup_gui.c`).

**Screen capture needs replacing.** `libs/scrap/src/quartz/` is built on
`CGDisplayStreamCreateWithDispatchQueue` — **macOS 10.8+**. It does not exist on
Leopard. A 10.5 capture path means `CGDisplayCapture` + `CGDisplayBaseAddress`,
hand-written. (Input injection is fine: `libs/enigo` uses Quartz Event Services
— `CGEventCreateKeyboardEvent`, `CGEventPost` — which is 10.4+.)

**Codec performance.** VP8/VP9/AV1 come from libvpx/aom as portable C, which
compiles for PPC — but the AltiVec/VSX paths are POWER8-little-endian, so a G5
gets plain C. VP8 is already selectable (`PreferCodec::VP8`), and it is the only
realistic choice. Expect low-resolution, low-framerate. Treat AV1 as absent.

**Big-endian.** RustDesk has never run big-endian. protobuf and libsodium are
endian-clean, but pixel-format conversion (`libs/scrap/src/common/convert.rs`),
and anything doing byte-level struct punning, is unaudited. This is the
silent-wrong-at-runtime class, and the only defence is measurement — the same
lesson `docs/build-ppc-mrustc.md` records for the libc structs.

**TLS is a maybe, not a no.** `ring 0.17` has no PowerPC assembly, but its
`build.rs` sets `asm_target = None` for any big-endian target and falls back to
portable C (the only `unsupported arch` panic is in the Windows-only `nasm()`
path). So rustls could plausibly build. Its correctness on big-endian is
untested by anyone, including upstream. Note the core RustDesk protocol uses
libsodium/NaCl, not TLS — TLS is only the hbbs HTTP/websocket side.

---

## 3b. Version levers — which older versions actually help

Evaluated after the first pass, because "use an older version" turns out to be
the highest-leverage question here.

### libc → `=0.2.174` — **yes, decisive**
Covered in §2c. Removes an apparent hard blocker for the cost of one pin.
Verified building for PowerPC.

### RustDesk 1.1.8 (edition 2018) instead of 1.4.9 — **yes, the biggest lever**
1.1.8 (`b0dfd777f`, 2021) has something current RustDesk does not: a **`cli`
feature** with a real headless peer session in `src/cli.rs` (connect, auth,
protocol loop, no window). `main.rs` selects it with
`#[cfg(not(any(..., feature = "cli")))]`.

Its dependency set is roughly half of today's. Absent entirely: `reqwest`,
`rustls`, `ring`, `tokio-tungstenite`, `tungstenite`, `webpki-roots`,
`rustls-platform-verifier`, `flutter_rust_bridge`, `tray-icon`, `tao`,
`portable-pty`, `kcp-sys`, `clipboard`, `whiteboard`, `hwcodec`, `zip`,
`totp-rs`, `qrcode-generator`. That deletes the entire TLS question (§3 "TLS is
a maybe") — 1.1.x speaks only the libsodium/NaCl protocol.

`sciter-rs` is still an unconditional dependency, but it is the **`dyn` branch**
— a pure-Rust binding that `dlopen`s libsciter at *runtime*. Under `cli` the UI
is never constructed, so it needs to transpile but never load, and it drops out
of a PPC manifest with a one-line edit. **The "GUI is impossible" blocker in §3
becomes "GUI is not needed."**

Costs: edition 2018 rather than 2021 (mrustc handles both), an older wire
protocol, and no upstream security fixes since 2021. For a G5-on-Leopard
proof-of-life that is the right trade.

### mrustc target ver 1.90 instead of 1.74 — **no**
Tempting, since the async-block `Future` impl was gated at 1.90. But auditing
every `TARGETVER_LEAST_1_90` site shows they are about `PointeeSized`/
`MetaSized`/`Destruct`, `format_args` internals, i128 emulation and the
`generator`→`coroutine` lang-item rename — essentially none of them async
lowering. Switching would mean rebuilding the whole PowerPC stdlib at 1.90 and
redoing the target port, to fix one line that fixes itself. **Stay at 1.74.**

### Older tokio — **no**
The remaining async gaps are inherent to any tokio, and 1.1.8's `hbb_common`
already used tokio 1.x. No leverage.

## 3c. Agent-only scope — G5 as the controlled machine

Goal narrowed: the G5 is the **remote guest** (you connect *into* it). That is
the `--server` role only. No viewer, no address book, no address-book UI.

### How the application is laid out

RustDesk is a single binary whose role is chosen by argv, in `src/core_main.rs`:

| argv | role | needed? |
|---|---|---|
| `--server` | **the agent** — accepts peers, captures, injects input | ✅ **this is all we want** |
| `--cm` | connection-manager **UI** — accept/reject prompt, session list | ⚠️ spawned *by* the server |
| *(none)* | main GUI (sciter/flutter): address book, connect box | ❌ |
| `--connect <id>` | viewer/client session | ❌ |
| `--service`, `--tray`, `--install`, `--elevate`, … | platform plumbing | ❌ |

The agent path, top to bottom:

```
rendezvous_mediator.rs   register with the ID server (hbbs) and punch NAT,
                         AND/OR direct_server() — a plain TCP listener
        ↓
server.rs                accept, own the service registry  (start_server(true, …))
        ↓
server/connection.rs     per-peer: handshake, NaCl encryption, password auth,
                         permissions, the message loop
        ↓ subscribes to
server/video_service.rs    scrap capture → I420 → VP8/VP9 → VideoFrame
server/input_service.rs    mouse/key events → enigo injection (+ cursor/pos publishers)
server/audio_service.rs    cpal → opus                      [droppable]
server/clipboard_service.rs                                 [droppable]
```

`start_server(true, _)` (1.1.8 `src/server.rs:265`) is the entire entry point:
start IPC, `fix_key_down_timeout_loop()`, `RendezvousMediator::start_all()`.
**It never touches the UI.**

### Yes — this is buildable as its own feature, and it is much smaller

Server-side code, 1.1.8-era vs today:

| | 1.1.8 | current | |
|---|---|---|---|
| `src/server/` files | **6** | 19 | no terminal/portable/printer/rdp/uinput/wayland/dbus/qos services |
| `src/server/` total | **~100 KB** | ~900 KB | **9× smaller** |
| `connection.rs` | 38 KB | 310 KB | 8× |
| `video_service.rs` | 15 KB | 48 KB | |
| `input_service.rs` | 22 KB | 84 KB | |
| CM coupling (`send_to_cm`/`start_ipc`) | 6 sites | 20 sites | 3× easier to stub |

1.1.8 registers exactly five services — audio, video, clipboard, input-cursor,
input-pos. Dropping audio and clipboard leaves video + input, which *is* the
feature you asked for.

### The commit to start from: `1931cb8c7` (2022-01-05, still 1.1.8)

This is the sweet spot, and it is not the same commit as §3b:

- has the **`cli` feature** (GUI-free entry point)
- has **`direct_server`** — added in *this* commit, so you get direct-IP access
  and **do not need to run an ID/rendezvous server at all**
- still the 6-file, ~100 KB `src/server/`

One correction to §3b worth stating plainly: the `cli` main() advertises
`-s, --server 'Start server'` in its usage string but **only implements
`--port-forward`**. The `--server` arm is not wired up. That is a ~5-line
addition calling `start_server(true, true)` — the point stands that `cli` gives
a GUI-free `main()`, you just finish it yourself.

### What must still be hand-written for the agent

1. **Leopard capture backend.** `libs/scrap/src/quartz/` is `CGDisplayStream*`
   at every version including this one — 10.8+, absent on Leopard. Needs a 10.5
   path (`CGDisplayCapture` + `CGDisplayBaseAddress`/`CGDisplayBytesPerRow`),
   feeding BGRA into the existing `convert.rs` → I420 → vpx chain. This is the
   single largest piece of new code, and it is small and self-contained.
2. **A CM stub.** `connection.rs` spawns `--cm` on every connection. Either
   satisfy the IPC with a no-op auto-accept, or cut the 6 call sites. Combine
   with a permanent password so nothing ever needs a prompt.
3. **Verify `enigo`'s macOS backend on 10.5.** It uses Quartz Event Services
   (`CGEventCreateKeyboardEvent`, `CGEventPost`, `CGEventSourceKeyState`),
   which are 10.4+, so this should mostly be checking rather than writing.

### What this does *not* avoid

The async work in §2 is unchanged — the agent is as tokio-bound as the rest of
the app. Narrowing to `--server` cuts dependency count and hand-written UI; it
does not cut `async {}`-inside-`async fn` or `Pin<Box<dyn Future>>`.

## 3d. Feature scope for the agent — decided

| feature | phase | notes |
|---|---|---|
| **video** (capture → VP8 → send) | **1** | the point of the exercise |
| **input** (keyboard/mouse injection) | **1** | `enigo`, Quartz Events are 10.4+ |
| **audio** | **1 — wanted** | `cpal` + `magnum-opus` deliberately kept in the manifest. libopus is portable C; cpal's macOS backend is CoreAudio AudioUnit, which exists on 10.4/10.5, but the vendored `open-trade/cpal` fork is unverified there. If it blocks the build, demote to phase 2 rather than dropping the deps. |
| **clipboard** | **2 — later** | `arboard` + `clipboard-master` kept in the manifest so `server/clipboard_service.rs` keeps compiling, but not a phase-1 target. Text first; file copy/paste is not in this vintage at all. |
| file transfer | 3 | present in 1.1.8, untested here |
| port forward | 3 | already the only thing the `cli` main implemented |
| terminal / printer / RDP / virtual display | never | not in this vintage |
| running as a launchd service | later | explicitly deferred; run it from a shell first |

### Self-hosted is a requirement, not an option
The agent must talk to a self-hosted hbbs/hbbr. Two independent paths, both
supported at this commit:
- **`direct_server`** — a plain TCP listener; the client connects by IP. No ID
  server at all. Simplest thing that can possibly work, and the reason
  `1931cb8c7` was chosen as the base.
- **`custom-rendezvous-server` + `key`** — the normal connect-by-ID flow
  against your own hbbs.

Both are settable without a GUI via the new flags (`--rendezvous-server`,
`--key`, `--password`, `--get-id`) — see `rustdesk-ppc/README.md`.

### The connection manager: auto-accept, with a hard password requirement
Upstream spawns a sciter window (`--cm`) per connection and *bails the
connection* if it never answers on the `_cm` IPC socket. `src/cm_headless.rs`
answers it instead: same IPC protocol, no window, replies `Data::Authorize`.

That means **it accepts every peer**, so the permanent password becomes the only
authentication. `--server` and `--cm` both refuse to start without one. See the
security note in `rustdesk-ppc/README.md`.

## 3e. Build log — where the port actually stands

Branch `ppc-agent`. Everything below is measured, on `powerpc-apple-darwin`
with `PPC_STUB_CC=1` (front-end only; no PowerPC Mac was reachable).

**Done**
- Branch cut from `1931cb8c7`; `hbb_common` un-submoduled (it was a plain
  directory at this vintage).
- Manifest deviations: `libc = "=0.2.174"`, `bytes = "=1.10.1"`, `sciter-rs`
  removed. **444 crates vendored**; the agent's macOS graph is **175 crates**
  (vs 376 for current RustDesk) with **no ring/rustls/openssl/reqwest/hyper** —
  only libsodium, as predicted in §3b.
- `rustdesk-ppc/` tooling: `build-ppc.sh`, `patches/git-deps.py` (minicargo
  cannot read `git =` deps), `stub-cc.sh`, `README.md`.
- `src/cm_headless.rs` + `--server`/`--cm`/`--password`/`--rendezvous-server`/
  `--key`/`--get-id` wired into the `cli` main (which upstream left as a
  port-forward-only stub, with a `crate::VERSION` path that never compiled).
- Host `cargo check` restored as a usable gate: `[profile.dev.build-override]
  debug-assertions = false` (this vintage's `protobuf-codegen-pure` trips modern
  std's UB checks) and three `let _ = LOCK.read()` → `drop(...)`
  (`let_underscore_lock` is deny-by-default now). `hbb_common` compiles clean.

**Five more mrustc fixes landed** (on branch `async-block-future-1.74`), on top
of the three from §2:
4. `hir_typeck/helpers.cpp` — `get_inner_type` now looks through `Pin<P>`, and a
   new `type_is_pin` + `m_lang_Pin` back it. Without this, a method with an
   arbitrary self type is invisible on a trait object:
   `No applicable methods for {Pin<&mut dyn Future>}.poll` (futures-task).
5. `hir_typeck/helpers.cpp` — the trait-object unsizing path hit an
   unconditional `TODO` even when the source and destination trait params were
   identical and no monomorphisation was owed. Now guarded on an actual
   difference. (`futures-task`, unsizing `Pin<Box<F>>` → `*mut dyn Future`.)

**Where it stops**
`futures-io v0.3.33` — `error:0: Unspecified lifetime in outer context`. 14
crates through, ~14% of 262. The next gap in the tail.

**Not yet started**
- The Leopard capture backend (`CGDisplayStream` is 10.8+).
- `scrap`'s build script: it wants `VCPKG_ROOT` and runs **bindgen** for the
  libvpx/libyuv FFI. Neither is viable under mrustc. Plan: generate
  `vpx_ffi.rs`/`yuv_ffi.rs` once on the host, check them in, and give scrap a
  minicargo *script-override* that only emits the link directives. `bindgen`
  already sets `layout_tests(false)` with the comment "breaks 32/64-bit compat",
  so cross-width reuse of the bindings is anticipated upstream.
- libvpx/libyuv/libopus built for PowerPC on the G5.

## 3f. Git-dependency audit (2026-08-04)

This vintage pulls eight crates from git forks. minicargo cannot read `git =`
deps at all, so each one costs a `patches/git-deps.py` rewrite — and each is an
unaudited fork of unknown vintage. Question asked: are any of them just
published crates by now?

Method: diff each vendored fork's `src/` against the same version from
crates.io.

| crate | fork ver | published | source delta | verdict |
|---|---|---|---|---|
| **cpal** | 0.13.4 | 0.13.4 | only `host/alsa/mod.rs` (Linux) + `host/asio/stream.rs` (Windows ASIO); **`host/coreaudio/` byte-identical** | ✅ **de-forked** — carries no change for a macOS/PowerPC target |
| psutil | 3.2.1 | 3.2.1 | **byte-identical** | ❌ keep — the fork exists to bump `platforms` off 0.2.1, and *every* `platforms` 0.2.x is now yanked, so the published crate cannot be resolved at all |
| parity-tokio-ipc | 0.7.3-6 | 0.7.0 max in series | 391 lines, incl. `unix.rs` | ❌ keep — real fork, no matching release |
| magnum-opus | 0.4.0 | 0.3.2 max | 1715 lines + a new `src/opus_ffi.rs` | ❌ keep — real fork, no matching release |
| confy | 0.4.0-2 | 0.4.0 | 498 lines in `src/lib.rs` | ❌ keep — real fork |
| tokio-socks | 0.5.1 | 0.5.1 | 175 lines + a whole new `src/udp.rs` | ❌ keep — fork adds UDP |
| systray | 0.4.1 | 0.4.0 max | — | n/a — Windows only, cfg'd out |
| rust-pulsectl | 0.2.12 | not published | — | n/a — Linux only, cfg'd out |

Net: **8 git deps → 7.** Modest, but cpal is on the audio path and one less
unaudited fork there is worth having. The two version-suffixed forks
(`0.4.0-2`, `0.7.3-6`) advertise their own divergence; the interesting result
was psutil, which *looks* trivially de-forkable and is not.

**Bonus finding on the audio path:** `magnum-opus` declares `mod opus_ffi;`,
i.e. it uses a **checked-in `src/opus_ffi.rs`**, while its `build.rs`
independently runs bindgen into `OUT_DIR` — output nothing reads. So its bindgen
build-dependency can be script-overridden away entirely, exactly the trick
`scrap` will need. One fewer reason to compile bindgen under mrustc.

## 4. What I'd actually do

The version analysis changes the recommendation. The original framing — "an
mrustc project first, a RustDesk project second" — still holds, but the mrustc
half is smaller than it first looked and the RustDesk half can be made much
smaller by choice of version.

### Recommended: RustDesk `1931cb8c7` (1.1.8), agent-only, on mrustc 1.74

A `rustdesk-ppc` manifest in the `rb-cli-ppc` style:

- base commit **`1931cb8c7`** — `cli` feature *and* `direct_server` (§3c)
- `--no-default-features --features cli`, with a `--server` arm added to the
  `cli` main() calling `start_server(true, true)`
- `libc = "=0.2.174"` — the §2c pin
- drop `sciter-rs` from the manifest (unused once the UI path is gone)
- drop the audio and clipboard services; keep video + input
- direct-IP access, so no self-hosted ID server is required
- reuse `ppc-cc-remote.py` / `ppc-ar-remote.py` / the PowerPC libstd unchanged

That eliminates, without writing any code: the GUI blocker, the TLS/ring
question, the flutter and tray stacks, the whole viewer/client half, and
roughly half the crate graph. What remains is genuinely about async and about
the platform.

**Compiler work still required, in order:**
1. ~~`Future` impl for async blocks gated at 1.90~~ — **done** (§2a)
2. ~~`$item:item` vs `pub async fn`~~ — **done** (§2e)
3. ~~`async fn` as a block-level item~~ — **done** (§2e)
4. **`async {}` nested inside an `async fn`** — MIR type-rewrite bug. The one
   real async blocker left, and the shape `tokio::spawn(async move { … })` takes.
5. **`Pin<&mut Self>` receivers** in `trans/auto_impls.cpp` — 1.1.8 also uses
   `async-trait`, so this is still needed.
6. **The tail** — `Parse_Impl_Item` interpolated `MacroInv` is next; expect more.
   Every gap so far has been small, local and loud, which is the same trajectory
   `docs/build-ppc-mrustc.md` records across its ~380 crates.

**Platform work still required** (independent of the compiler):
- a 10.5 screen-capture path (`CGDisplayCapture` + `CGDisplayBaseAddress`)
- big-endian audit of pixel conversion
- VP8-only codec configuration

### Do first, cheaply: establish the perf ceiling
Get a G5 (10.5) reachable again — `ssh ppctiger` currently fails with
`Permission denied (publickey)` — and measure libvpx VP8 decode at 800×600 and
1024×768 with plain C, `-mcpu=970`. If that can't hold ~10 fps it reframes
everything above, and it costs an afternoon.

### Fallback if the async tail doesn't converge
A purpose-built blocking-IO PowerPC client: RustDesk's wire protocol is
protobuf over TCP with NaCl encryption — endian-clean and needing no async at
all. It reuses `libs/vintage_common/protos/` (the actual contract), the proven
mrustc→C→gcc pipeline, and `rusty-backup`'s `ppc-tiger/` GUI scaffolding. Worth
holding in reserve rather than starting with, now that libc and the `cli`
feature have shrunk the direct port so much.

---

## 5. Summary table

Assuming the recommended configuration (1.1.8 + `cli` + libc 0.2.174 + mrustc 1.74):

| Axis | State |
|---|---|
| Toolchain (mrustc→C→gcc10→Mach-O) | ✅ proven, reusable |
| PowerPC libstd (G3/G4/G5) | ✅ built |
| tokio's whole dependency tree for PPC | ✅ **transpiles** (10 crates, verified) |
| `async fn` / `.await` | ✅ works |
| `async {}` blocks | ✅ fixed here |
| `pub async fn` in `:item` capture | ✅ fixed here |
| `async fn` in a function body | ✅ fixed here |
| libc | ✅ pin `=0.2.174` |
| GUI | ✅ sidestepped — `cli` feature is headless |
| TLS (ring/rustls) | ✅ sidestepped — not a 1.1.x dependency |
| Input injection | ✅ Quartz Events are 10.4+ |
| Nested async block in async fn | ❌ MIR bug — **the remaining async blocker** |
| `Pin<Box<dyn Future>>` (`async-trait`) | ❌ unimplemented |
| tokio's own parser-gap tail | ⚠️ small and loud, but unbounded in count |
| Screen capture on 10.5 | ⚠️ CGDisplayStream is 10.8+; hand-write |
| Codecs | ⚠️ VP8 only, portable C, perf unmeasured |
| Big-endian correctness | ⚠️ entirely unaudited |

---

## Reproducing the async measurements

```bash
cd ~/repos/mrustc
# async fn — works
MRUSTC_TARGET_VER=1.74 ./bin/mrustc test.rs --edition 2021 -L output-1.74.0 -o out
# async block — needs the static.cpp:426 gate fix
git checkout async-block-future-1.74 && make -j$(nproc)
```

Minimal tokio-for-PPC probe (deferred codegen, no PowerPC Mac needed):

```bash
MRUSTC_TARGET_VER=1.74 MINICARGO_DEFER_CODEGEN=1 ./bin/minicargo <crate> \
  --vendor-dir <crate>/vendor --target powerpc-apple-darwin \
  -L output-1.74.0-powerpc-apple-darwin-g5 --output-dir <out>
```
