# Running the agent on Solaris 9 / SPARC

`README.md` and `RESUME.md` in this directory are development notes, and their
build sections describe the **Solaris 10** arrangement, where the C was compiled
on the Blade itself over ssh. That is gone. Solaris 9 has no usable compiler, so
the agent is cross-compiled on Linux and only *run* on the Blade.

This file is the short version: build it, install it, configure it, check it.

---

## 1. Build it

Prerequisites, all outside this repository:

| | |
|---|---|
| cross toolchain | GCC 4.9.4 for `sparcv9-sun-solaris2.9` — `mrustc/docker/sol9-cross`, or `~/sol9-toolchain/opt` |
| Solaris 9 sysroot | `~/sol9-toolchain/sysroot` — headers and libraries pulled off a live install |
| mrustc stdlib | `mrustc/output-1.74.0-sparcv9-sun-solaris2.9` |
| C libraries | `~/sol9-deps/prefix` — libsodium, zlib, zstd, mbedTLS, libvpx, all static |

Then:

```sh
./scripts/build-sol9.sh
```

The binaries land in `target/sparcv9-sun-solaris2.9/`: `rustdesk-agent` itself,
plus `testpeer`, `captest`, `prototest` and `convtest`. Every path the script
needs can be overridden by an environment variable of the same name — read its
header.

**What has to be on the target.** Only the one binary. libsodium, mbedTLS, zstd
and libvpx are linked statically; what is left is Solaris' own libraries plus
X11:

```
libX11.so.4  libXext.so.0  libXtst.so.1      (RPATH /usr/openwin/lib/sparcv9)
libc libm librt libpthread libsocket libnsl libresolv libdl libsendfile liblgrp
libgcc_s.so.1
```

Note what is *absent*: no `libXdamage`, no `libXfixes`. Solaris 9 has neither,
and `build.rs` gates them out by probing for the headers.

`libgcc_s.so.1` is the one thing Solaris 9 does not ship and the agent cannot do
without — Rust's `unwind` crate asks for it by name, which defeats
`-static-libgcc`. Install the toolchain's copy once:

```sh
scp ~/sol9-toolchain/opt/sparcv9-sun-solaris2.9/lib/sparcv9/libgcc_s.so.1 HOST:/tmp/
ssh HOST 'sudo cp /tmp/libgcc_s.so.1 /usr/lib/sparcv9/ && sudo chmod 755 /usr/lib/sparcv9/libgcc_s.so.1'
```

## 2. Install it

```sh
scp target/sparcv9-sun-solaris2.9/rustdesk-agent HOST:/tmp/
ssh HOST 'sudo mkdir -p /usr/local/bin \
       && sudo cp /tmp/rustdesk-agent /usr/local/bin/ \
       && sudo chmod 755 /usr/local/bin/rustdesk-agent'
```

There is no service manifest, no init script and no packaging. Solaris 9 predates
SMF, so if you want it started at boot it wants an `/etc/init.d` script and an
`rc3.d` link, written by hand.

## 3. Configure it

Configuration is a flat `key = value` file at `~/.rustdesk-ppc-agent.conf`, mode
`0600`, holding six scalars — id, password, salt, and the Ed25519 keypair whose
secret half is why the mode matters. It is created on first use. Everything is
set through the binary rather than by editing the file:

```sh
rustdesk-agent --password SECRET     # at least 6 characters. Required.
rustdesk-agent --show-id             # the 9-character agent ID
rustdesk-agent --show-key            # the public key a peer needs
```

That is the whole of a direct-IP setup. Then just:

```sh
DISPLAY=:0 rustdesk-agent --log info
```

It listens on `0.0.0.0:21118` (`--listen`, `--port`) and answers LAN-discovery
broadcasts on udp/21119, so it appears in a client's local-network list by
hostname.

**Optional, and independent of each other:**

```sh
rustdesk-agent --server HOST         # register with a rendezvous server, so the
                                     # machine is reachable by ID from anywhere
rustdesk-agent --key KEY             # only if the relay was started with -k
rustdesk-agent --api-server URL      # report in to a console, so the machine
                                     # appears in its device list
```

Both are persisted and both take a `--no-…` form to undo. `--server` makes the
machine *reachable*; `--api-server` makes it *visible*. Neither implies the
other. `--ca-bundle PATH` is there for a console behind a private CA.

`--secure` requires the signed-identity exchange and is **off** by default: a
client connecting by IP does not take part in it, and turning it on there
deadlocks the handshake.

`--log debug` (`-v`) or `--log trace` (`-vv`) prints every message and frame,
which is the fastest way to find where a client diverges.

## 4. The display, which is the hard part on this machine

The agent captures a real X display and Solaris 9 gives you exactly one, on the
console. Two things will stop it dead, both silently:

* **The CDE greeter holds a server grab.** While `dtlogin` is at its login
  screen, every X client blocks *inside `XOpenDisplay`* — no error, no timeout.
  Either log in on the console properly, or stop it:
  `sudo /etc/init.d/dtlogin stop`, and start it again afterwards.
* **There is no virtual framebuffer.** `/usr/openwin/bin/Xvfb` is a wrapper for
  `Xsun -dev vfb` and the vfb module is not installed, so there is nothing to
  fall back to. The Solaris 10 port ran against Xvfb; this one cannot.

To drive the framebuffer yourself, with no greeter in the way:

```sh
sudo /etc/init.d/dtlogin stop
sudo /usr/openwin/bin/Xsun :1 -ac -nobanner -dev /dev/fbs/jfb0 defdepth 24
```

`Xsun` dies when the ssh session that started it ends, whatever `nohup` is told
to do, so the server and everything under it have to live inside a **single**
`ssh` invocation. `scripts/sol9-console-test.sh` does that, and hands the console
back to CDE when it is finished.

Two quirks of this framebuffer worth knowing:

* Every TrueColor visual the XVR-600 offers is **BGR** — red in the low byte.
  `capture_shim.c` detects it and swaps the channels as the canvas is filled.
  Black-and-white test content cannot show whether that is working, because it
  is symmetric under the swap; put a known colour on screen.
* `Xsun` advertises no **DAMAGE** and no **XFIXES**. Capture therefore compares
  canvases instead of subscribing to damage, the clipboard polls, and the remote
  cursor keeps whatever shape the peer last drew.

## 5. Check it

`--probe-display` reports what the framebuffer looks like and benchmarks the
capture, conversion and encode path end to end without any networking. It runs a
full VP8 tuning sweep, so give it a few minutes.

`testpeer` is the real check: it speaks the actual protocol — signed identity,
sealed key, password hash, login — and then counts and decodes the video that
comes back.

```sh
testpeer 127.0.0.1:21118 SECRET 15
```

`scripts/sol9-console-test.sh` is the two of them together, start to finish —
stop the greeter, bring up `Xsun` on the framebuffer, put a known colour on
screen, run the agent, point `testpeer` at it, and restore CDE:

```sh
scp scripts/sol9-console-test.sh HOST:/tmp/
ssh HOST 'sh /tmp/sol9-console-test.sh PASSWORD'
```

What a good run looks like, from this machine on 2026-09-02:

```
[  0.015] INFO  agent listening on 0.0.0.0:21118 (id apigvbxij)
[  0.015] INFO  lan discovery listening on udp/21119
[  6.017] INFO  peer 'testpeer' (testpeer) logged in -- entering message loop
[  6.019] INFO  encoder: 640x512, 375 kbps, 1 thread(s) of 1 processor(s)
[  6.040] INFO  video: 1280x1024 framebuffer served at 640x512 (1/2)
[  6.040] INFO  capture path: MIT-SHM, full screen per poll  -- NO DAMAGE
               TRACKING: every frame will be a full-screen read
cursor: XFIXES absent; falling back to a drawn arrow

  video frames   12 (1 key), 6640 bytes total
  first frame    287 ms after login
VERDICT: the agent is serving video to a peer.
```

Those last two log lines are the extension gating showing up at runtime, and
they are expected here rather than a fault. The frame rate a run reports is
change-driven and says more about what was moving on screen than about the
machine — `captest` measures the actual ceiling, which is about 40fps.
