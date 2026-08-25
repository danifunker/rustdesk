#!/bin/sh
#
# sysprobe.sh -- everything about the Sun Blade that the port's next decisions
# depend on. Copy it over and run it; paste the output back.
#
#   scp probes/sysprobe.sh blade:/tmp && ssh blade 'sh /tmp/sysprobe.sh'
#
# Written for Solaris 10's /bin/sh, which is the legacy Bourne shell: backticks
# rather than $(), no `local`, no [[ ]], no `echo -n`.
#
# The question it exists to answer is the C compiler one. mrustc emits C11 --
# <stdatomic.h>, __int128, _Static_assert -- and Solaris 10's own
# /usr/sfw/bin/gcc is 3.4.3, which has none of them. Everything else here is
# context; the compiler section is the gate.

echo "===================== machine ====================="
uname -a
isainfo -kv 2>/dev/null
echo "--- release ---"
cat /etc/release 2>/dev/null | head -3
echo "--- cpu ---"
psrinfo -v 2>/dev/null | head -12
echo "--- memory ---"
prtconf 2>/dev/null | grep -i memory
echo "--- model ---"
prtdiag 2>/dev/null | head -5

echo
echo "===================== disk ======================="
df -k /tmp $HOME 2>/dev/null

echo
echo "===================== compilers =================="
# The test program uses exactly what mrustc's output needs.
cat > /tmp/c11probe.c <<'EOF'
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
typedef __int128 i128;
static _Atomic long long g;
static i128 wide(i128 a) { return a * 3; }
_Static_assert(sizeof(i128) == 16, "int128");
int main(void) {
    atomic_fetch_add(&g, 7);
    printf("ok atomic=%lld int128=%d align=%d endian=%s\n",
           (long long)g, (int)sizeof(i128), (int)__alignof__(i128),
           (*(char *)(int[]){1}) ? "little" : "big");
    return (int)wide(2);
}
EOF

SEEN=""
for CC in /usr/sfw/bin/gcc /opt/csw/bin/gcc /usr/local/bin/gcc /usr/bin/gcc \
          /opt/SUNWspro/bin/cc /opt/solarisstudio12.3/bin/cc gcc cc
do
    FOUND=`command -v "$CC" 2>/dev/null`
    if [ -z "$FOUND" ]; then continue; fi
    # `gcc` and /usr/bin/gcc are usually the same binary; report each once.
    case " $SEEN " in *" $FOUND "*) continue ;; esac
    SEEN="$SEEN $FOUND"
    echo "--- $FOUND ---"
    "$FOUND" --version 2>/dev/null | head -1 || "$FOUND" -V 2>&1 | head -1
    for BITS in "" "-m64"
    do
        rm -f /tmp/c11probe
        if "$FOUND" $BITS -std=gnu11 -o /tmp/c11probe /tmp/c11probe.c 2>/tmp/c11probe.err
        then
            OUT=`/tmp/c11probe 2>&1`
            echo "    C11 ${BITS:-default}: BUILDS, runs: $OUT"
            file /tmp/c11probe 2>/dev/null | sed 's/^/      /'
        else
            echo "    C11 ${BITS:-default}: FAILS -- `head -1 /tmp/c11probe.err`"
        fi
    done
done
echo "--- package tools (for installing a newer gcc) ---"
command -v pkgutil pkgadd pkg wget curl 2>/dev/null

echo
echo "===================== linker / ar ================"
for T in /usr/ccs/bin/ld /usr/ccs/bin/ar /usr/local/bin/ld /opt/csw/bin/gar
do
    if [ -x "$T" ]; then echo "--- $T ---"; "$T" -V 2>&1 | head -2; fi
done

echo
echo "===================== X11 ========================"
echo "--- running server (egrep: Solaris grep has no \\| alternation) ---"
ps -ef 2>/dev/null | egrep 'Xsun|Xorg|Xserver|dtlogin|dtgreet' | grep -v egrep
echo "--- server binaries ---"
ls -l /usr/openwin/bin/Xsun /usr/X11/bin/Xorg 2>/dev/null
echo "--- 32-bit libraries ---"
ls /usr/openwin/lib/libX11.so* /usr/openwin/lib/libXext.so* \
   /usr/openwin/lib/libXtst.so* /usr/openwin/lib/libXfixes.so* 2>/dev/null
echo "--- 64-bit libraries (decides whether the agent can be sparcv9) ---"
ls /usr/openwin/lib/sparcv9/libX11.so* /usr/openwin/lib/sparcv9/libXext.so* \
   /usr/openwin/lib/sparcv9/libXtst.so* /usr/openwin/lib/sparcv9/libXfixes.so* 2>/dev/null
echo "--- headers ---"
ls /usr/openwin/include/X11/Xlib.h \
   /usr/openwin/include/X11/extensions/XShm.h \
   /usr/openwin/include/X11/extensions/XTest.h \
   /usr/openwin/include/X11/extensions/Xfixes.h 2>/dev/null
echo "--- graphics device ---"
ls -l /dev/fbs 2>/dev/null | head -5
fbconfig -list 2>/dev/null | head -5

echo
echo "===================== libraries the agent wants ==="
ls /opt/csw/lib/libsodium* /usr/local/lib/libsodium* 2>/dev/null || echo "  no libsodium (we will build it)"
ls /opt/csw/lib/libvpx* /usr/local/lib/libvpx* 2>/dev/null || echo "  no libvpx (we will build it, or start without VP8)"

echo
echo "done."
