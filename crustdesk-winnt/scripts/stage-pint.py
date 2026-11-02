#!/usr/bin/env python3
"""Stage C-Desk-Vint as an app of PINT (Windows NT 4.0 for ARM, github.com/tenox7/PINT).

No compiler ever ran on NT for ARM, so this build is not made on the machine:
PINT's build (Microsoft C for ARM in Docker) compiles it as one of its own
apps, as it does TenoxVNC and Internet Explorer, with NT 4.0's C run-time and
import libraries. scripts/pint-build.sh drives that build.

The tree is build/stage (scripts/stage.py: the same rewrites as on every other
processor), flattened. PINT's build finds a source by its bare name in the
component's directory or its parent (build/srclist.sh), and its incfix.sed
cuts the directories out of a source's includes, so everything is put in one
directory, cdv/, and the quoted includes of the headers are cut the same way
(no two files of the tree share a name; checked here). The components are
directories under it, each a SOURCES file:

    cdv/sodium   libsodium-min            LIBRARY cdvnacl
    cdv/bssl     BearSSL                  LIBRARY cdvbssl
    cdv/zstd     the zstd decoder         LIBRARY cdvzstd
    cdv/exe      the agent: src/, core    PROGRAM cdv (CDV.EXE: C-Desk-Vint.exe)
    cdv/ntexec   tools/nt/ntexec.c        PROGRAM ntexec (scripts/ntx.py's server)

    stage-pint.py [OUT]     (default build/pint; OUT/cdv goes to PINT's apps/cdv)
"""
import os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(HERE, "build", "stage")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "pint")
CDV = os.path.join(OUT, "cdv")

R_INC = re.compile(r'^([ \t]*#[ \t]*include[ \t]*")([^"]*[\\/])?([^"\\/]+)"', re.M)

# the stage's own flags (stage.py), as SOURCES macros
COMMON = "-DCONFIGURED=1 -DSODIUM_STATIC"
BSSL = ("-DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_RDRAND=0 "
        "-DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0")
ZSTD = "-DHUF_FORCE_DECOMPRESS_X1 -DZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT -DZSTD_NO_INLINE"


def files(sub, ext):
    out = []
    for root, dirs, names in os.walk(os.path.join(STAGE, sub)):
        dirs.sort()
        out += [os.path.join(root, n) for n in sorted(names) if n.endswith(ext)]
    return out


def sources(comp, kind, name, srcs, defines="", flags="", extra=""):
    d = os.path.join(CDV, comp)
    os.makedirs(d, exist_ok=True)
    lines = ["# Written by crustdesk-winnt/scripts/stage-pint.py.",
             "MAJORCOMP=apps", "MINORCOMP=cdv", "",
             "TARGETNAME=%s" % name, "TARGETPATH=obj", "TARGETTYPE=%s" % kind, "",
             "USE_LIBCMT=1", "INCLUDES=..", ""]
    if defines:
        lines.append("C_DEFINES=%s" % defines)
    # compat.h first in every file, as /FIcompat.h in the stage's makefile
    lines.append("USER_C_FLAGS=-FIcompat.h %s" % flags)
    lines += ["", "SOURCES=" + "".join(" \\\n\t%s" % s for s in srcs), ""]
    if extra:
        lines.append(extra)
    open(os.path.join(d, "sources"), "w", newline="\r\n").write("\n".join(lines) + "\n")


def main():
    env = dict(os.environ)
    subprocess.check_call([sys.executable, os.path.join(HERE, "scripts", "stage.py"), STAGE], env=env)
    if os.path.isdir(CDV):
        shutil.rmtree(CDV)
    os.makedirs(CDV)
    seen = {}
    every = files("", (".c", ".h", ".rc", ".ico")) + \
        [os.path.join(HERE, "tools", "nt", n) for n in ("ntexec.c",)]
    for p in every:
        n = os.path.basename(p).lower()
        if n in seen:
            sys.exit("stage-pint: %s and %s have one name" % (seen[n], p))
        seen[n] = p
        data = open(p, "rb").read()
        if n.endswith((".c", ".h", ".rc")):
            text = data.decode("latin-1")
            text = R_INC.sub(lambda m: m.group(1) + m.group(3).lower() + '"', text)
            data = text.encode("latin-1")
        open(os.path.join(CDV, n), "wb").write(data)

    base = lambda ps: [os.path.basename(p).lower() for p in ps]
    sources("sodium", "LIBRARY", "cdvnacl", base(files("nacl", ".c")), COMMON)
    sources("bssl", "LIBRARY", "cdvbssl", base(files("bssl", ".c")), BSSL)
    sources("zstd", "LIBRARY", "cdvzstd", ["zstddeclib.c"], ZSTD,
            # its generic C, not the intrinsics of a Microsoft C it would take
            # for one it knows (stage.py: /U_MSC_VER)
            "-U_MSC_VER")
    agent = base(files("nt", ".c")) + base(files("core", ".c"))
    sources("exe", "PROGRAM", "cdv", agent + ["cdv.rc"], COMMON,
            extra="UMTYPE=windows\nUMENTRY=winmain\n\n"
                  "TARGETLIBS=..\\sodium\\obj\\*\\cdvnacl.lib \\\n"
                  "\t..\\bssl\\obj\\*\\cdvbssl.lib \\\n"
                  "\t..\\zstd\\obj\\*\\cdvzstd.lib \\\n"
                  "\t$(BASEDIR)\\public\\sdk\\lib\\*\\wsock32.lib \\\n"
                  "\t$(BASEDIR)\\public\\sdk\\lib\\*\\ole32.lib\n")
    sources("ntexec", "PROGRAM", "ntexec", ["ntexec.c"],
            extra="UMTYPE=console\n\nTARGETLIBS=$(BASEDIR)\\public\\sdk\\lib\\*\\wsock32.lib\n")
    print("staged %s: %d files, components sodium bssl zstd exe ntexec" % (CDV, len(seen)))


if __name__ == "__main__":
    main()
