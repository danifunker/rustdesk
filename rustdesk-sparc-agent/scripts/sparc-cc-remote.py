#!/usr/bin/env python3
"""A drop-in `cc` that compiles on the Sun Blade over ssh.

mrustc is a transpiler: Rust in, C99 out. Something has to turn that C into
SPARC objects, and until a `sparc-sun-solaris2.10` cross-gcc exists here, the
only compiler that can is the one on the Solaris box. Rather than run the
pipeline in two manual halves, this script *is* the compiler as far as mrustc
is concerned: point `CC_sparcv9_sun_solaris` at it and every codegen step
transparently ships its `.c` to the Blade, runs gcc there, and copies the `.o`
back. minicargo's dependency graph, parallelism and incremental rebuilds all
keep working.

    export SPARC_HOST=user@blade
    export SPARC_CC=/opt/csw/bin/gcc      # needs C11 <stdatomic.h>: gcc >= 4.9
    export CC_sparcv9_sun_solaris=/path/to/scripts/sparc-cc-remote.py

mrustc invokes the compiler as a single shell string with every argument in a
response file (`cc @cmdfile`), so that form is handled here too.

Environment:
  SPARC_HOST          ssh destination (required), e.g. dani@blade
  SPARC_CC            remote compiler (default /opt/csw/bin/gcc). Solaris 10's
                      own /usr/sfw/bin/gcc is 3.4.3 and has no <stdatomic.h>,
                      so it cannot build what mrustc emits.
  SPARC_REMOTE_ROOT   remote mirror of the local build tree (default sparc-xbuild)
  SPARC_REMOTE_PATH   PATH for the remote commands. Default puts /opt/csw/bin
                      first: an ssh command runs a non-login shell, whose PATH
                      has neither the OpenCSW gcc nor rsync in it.
  SPARC_CFLAGS        extra flags prepended to every compile
  SPARC_LDFLAGS       extra flags appended to every link (see DEFAULT_LDFLAGS)
  SPARC_CC_VERBOSE    set to echo each remote command
  SPARC_BIG_TU_BYTES  a .c above this size gets BIG_TU_ARGS (default 8 MiB;
                      0 disables). gcc's peak memory scales with the whole unit,
                      and OpenCSW's cc1 is a *32-bit* SPARC32PLUS binary -- it
                      runs out of address space (~4 GB) long before the Blade
                      runs out of RAM. libstd's unit is 31 MB, libcore's 27 MB.
  SPARC_BIG_TU_ARGS   what to use instead for those (default -O0 plus a much
                      more aggressive garbage collector)

This starts deliberately simple. The PowerPC equivalent grew a long tail of
workarounds (oversized-unit splitting, branch islands); SPARCv9 should need
fewer of them -- `call` reaches +/-2 GB and the address space is 64-bit -- but
add them here when a real failure asks for them, not before.
"""

import os
import shlex
import subprocess
import sys

HOST = os.environ.get("SPARC_HOST")
REMOTE_CC = os.environ.get("SPARC_CC", "/opt/csw/bin/gcc")
REMOTE_ROOT = os.environ.get("SPARC_REMOTE_ROOT", "sparc-xbuild")
REMOTE_PATH = os.environ.get("SPARC_REMOTE_PATH",
                             "/opt/csw/bin:/usr/ccs/bin:/usr/bin:/usr/sbin")
EXTRA_CFLAGS = shlex.split(os.environ.get("SPARC_CFLAGS", ""))
VERBOSE = bool(os.environ.get("SPARC_CC_VERBOSE"))

# What a Rust libstd needs on Solaris 10 beyond libc:
#   -lsocket -lnsl   the BSD socket calls live outside libc here
#   -lrt             POSIX timers/semaphores
#   -lpthread        std's threads (gcc's -pthread also implies -D_REENTRANT)
DEFAULT_LDFLAGS = "-lsocket -lnsl -lrt -lpthread"
LDFLAGS = shlex.split(os.environ.get("SPARC_LDFLAGS", DEFAULT_LDFLAGS))

BIG_TU_BYTES = int(os.environ.get("SPARC_BIG_TU_BYTES", 8 * 1024 * 1024))
BIG_TU_ARGS = shlex.split(os.environ.get(
    "SPARC_BIG_TU_ARGS",
    "-O0 --param ggc-min-expand=10 --param ggc-min-heapsize=32768"))
# Replaced when BIG_TU_ARGS takes over: mrustc emits -O1, and -O1 is what makes
# gcc hold the whole unit live.
OPT_FLAGS = ("-O", "-O0", "-O1", "-O2", "-O3", "-Os", "-Ofast")

# A path under one of these is a *remote* path (a Solaris header or library),
# never something to upload from here. Without this, `-L/usr/lib` would push a
# Linux userland onto the Blade.
SYSTEM_PREFIXES = ("/usr/", "/lib/", "/opt/", "/bin/", "/sbin/", "/etc/", "/var/",
                   "/platform/", "/devices/")
INPUT_SUFFIXES = (".c", ".h", ".o", ".a", ".s", ".S")


def die(msg):
    sys.stderr.write("sparc-cc-remote: %s\n" % msg)
    sys.exit(1)


def expand_args(argv):
    """mrustc passes `@file`; the file holds every argument, quoted, on one line."""
    out = []
    for a in argv:
        if a.startswith("@"):
            with open(a[1:]) as f:
                out.extend(shlex.split(f.read()))
        else:
            out.append(a)
    return out


def is_system(path):
    return any(path.startswith(p) for p in SYSTEM_PREFIXES)


def remote_path(local):
    """Mirror an absolute local path under the remote root, keeping its shape so
    diagnostics and debug info stay readable."""
    return "%s/%s" % (REMOTE_ROOT, os.path.abspath(local).lstrip("/"))


def run(cmd, **kw):
    if VERBOSE:
        sys.stderr.write("+ %s\n" % " ".join(shlex.quote(c) for c in cmd))
    return subprocess.run(cmd, **kw)


def main():
    if not HOST:
        die("SPARC_HOST is not set")

    args = expand_args(sys.argv[1:])
    if not args:
        die("no arguments")

    compiling = "-c" in args
    output = None
    uploads = []       # (local, remote)
    remote_dirs = set()
    new_args = []

    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o" and i + 1 < len(args):
            output = args[i + 1]
            r = remote_path(output)
            remote_dirs.add(os.path.dirname(r))
            new_args.extend(["-o", r])
            i += 2
            continue
        # A local input file: upload it and refer to it remotely.
        if (not a.startswith("-") and a.endswith(INPUT_SUFFIXES)
                and os.path.exists(a) and not is_system(os.path.abspath(a))):
            r = remote_path(a)
            uploads.append((a, r))
            remote_dirs.add(os.path.dirname(r))
            new_args.append(r)
            i += 1
            continue
        # -I/-L pointing into the local tree (a build script's OUT_DIR, say):
        # mirror the directory it names. System prefixes pass through untouched.
        if a[:2] in ("-I", "-L") and len(a) > 2:
            d = a[2:]
            if os.path.isdir(d) and not is_system(os.path.abspath(d)):
                r = remote_path(d)
                for name in sorted(os.listdir(d)):
                    p = os.path.join(d, name)
                    if os.path.isfile(p) and p.endswith(INPUT_SUFFIXES):
                        uploads.append((p, os.path.join(r, name)))
                remote_dirs.add(r)
                new_args.append(a[:2] + r)
                i += 1
                continue
        new_args.append(a)
        i += 1

    # An oversized translation unit gets its own flags.
    big = False
    for local, _ in uploads:
        if (BIG_TU_BYTES and local.endswith(".c")
                and os.path.getsize(local) > BIG_TU_BYTES):
            big = True
    if big:
        new_args = [a for a in new_args if a not in OPT_FLAGS] + BIG_TU_ARGS

    cmd = [REMOTE_CC] + EXTRA_CFLAGS + new_args
    if not compiling:
        cmd += LDFLAGS

    # Ship the inputs. One rsync for the lot: on a 1 Gb link the 33 MB libstd.c
    # is a few seconds, and rsync skips what is already current.
    if uploads:
        for local, r in uploads:
            remote_dirs.add(os.path.dirname(r))
    if remote_dirs:
        mk = "mkdir -p " + " ".join(shlex.quote(d) for d in sorted(remote_dirs))
        p = run(["ssh", HOST, mk])
        if p.returncode != 0:
            die("could not create remote directories")
    for local, r in uploads:
        p = run(["rsync", "-e", "ssh", "-t",
                 "--rsync-path=%s/rsync" % REMOTE_PATH.split(":")[0],
                 local, "%s:%s" % (HOST, r)])
        if p.returncode != 0:
            # rsync may not be on the Blade; scp always is.
            p = run(["scp", "-q", local, "%s:%s" % (HOST, r)])
            if p.returncode != 0:
                die("upload of %s failed" % local)

    remote_cmd = "PATH=%s; export PATH; %s" % (
        REMOTE_PATH, " ".join(shlex.quote(c) for c in cmd))
    p = run(["ssh", HOST, remote_cmd])
    if p.returncode != 0:
        sys.exit(p.returncode)

    # Bring the result home.
    if output:
        r = remote_path(output)
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        p = run(["scp", "-q", "%s:%s" % (HOST, r), output])
        if p.returncode != 0:
            die("could not fetch %s" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
