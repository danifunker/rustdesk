# iris: the X server xdm starts still wedges (a second case, distinct from the hostr fix)

Upstream `02c4e155` ("fix hostr readback issues") fixed the wedge our **capture
load** used to provoke, and it fixed it properly: the agent now runs sustained
`SGICapQueryCopyAndReset` traffic for a hundred seconds at a time with no
trouble at all, where before it managed roughly one frame.

There is a second one left, and it is the original: **the X server xdm starts
stops answering, without any client of ours involved.**

## Reproduction

`ports/iris-run/guest/xdm-check.sh` in `rustdesk/rustdesk-irix65-agent` does the
whole thing in one run. By hand, on IRIX 6.5.22m / IP22 / R5000 under
`iris --config iris.toml --ci --ci-display --cpu r5000`:

```sh
# kill whatever owns :0
for p in `ps -e -o pid,args | grep -E "Xsgi|X11/xdm" | grep -v grep | awk '{print $1}'`; do
    kill -9 $p
done
/etc/init.d/xdm start
sleep 60
```

xdm starts the server as

```
/usr/bin/X11/Xsgi -bs -nobitscale -c -pseudomap 4sight -solidroot sgilightblue
```

The light-blue root paints — you can see it in the window — and then everything
stops.

## What it looks like

```
$ ps -e -o pid,time,args | grep Xsgi
   5558   0:02  /usr/bin/X11/Xsgi -bs -nobitscale -c -pseudomap 4sight -solidroot sgilightblue

$ DISPLAY=:0 tmo 25 xdpyinfo
[killed after 25s]

$ ps -e -o pid,time,args | grep Xsgi        # 30 seconds later
   5558   0:02  /usr/bin/X11/Xsgi -bs -nobitscale -c -pseudomap 4sight -solidroot sgilightblue
```

The process is alive, **its accumulated CPU time does not move**, and IRIX's own
`xdpyinfo` hangs rather than being refused. That last part is the important one:
an X server that refuses a client still *answers* it. This one is not answering
anybody, which is the signature of a process asleep in the kernel making no
syscalls — the same picture as the pre-`02c4e155` wedge.

Two differences from that one:

- **No `ng1 pixel dma read timeout` on the serial console this time.** The
  kernel warning was always an occasional symptom rather than the whole story.
- **`iris-ci screenshot` returns an all-black 1282x1024 framebuffer**, even
  though the light-blue root had visibly painted a moment earlier.

The one thing in the emulator log that looks unusual is a repeating pair:

```
BT445: Sync-on-Green (IOG) DISABLED
BT445: Sync-on-Green (IOG) ENABLED
BT445: Sync-on-Green (IOG) DISABLED
BT445: Sync-on-Green (IOG) ENABLED
```

which does not appear when a bare `Xsgi :0 -bs -c` is running. Whether that is
the cause or another symptom of the same stall, we cannot tell from this side.

## Why it matters to us

A bare `Xsgi :0 -bs -c` — what `/root/restart-x.sh` starts — is completely
stable, and every performance measurement we have was taken against one. But
nobody runs a workstation that way. The things we cannot test until this is
fixed are exactly the ones that only exist in a real session:

- the agent surviving the X server restart xdm does at every logout, which on
  this platform is the recovery path that matters most;
- keyboard injection with a window manager running and something focused;
- capture volume on a real 4Dwm desktop rather than one xterm.

So this is not a cosmetic difference between two ways of starting X. It is the
difference between "the agent is fast" and "the agent works on a machine
somebody uses".

Happy to run anything you want on this image — it boots in about five minutes
and the harness to drive it is scripted.
