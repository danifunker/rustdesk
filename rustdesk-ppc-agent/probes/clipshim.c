/* Drive the real clipboard shim, at the flags it actually ships with.
 *
 * BACKLOG item 1e: `gcc10-bootstrap` for this target miscompiles at -O2, and a
 * change to `input_shim.c` once crashed the agent on a user's first click. So a
 * new shim gets exercised on the machine before it is deployed, and it gets
 * exercised as the *same object the agent links* rather than as a
 * reimplementation -- hence the #include of the source.
 *
 * Also the only way to see the shim's failure path, which is the one that will
 * actually run: outside the Aqua session every call returns -1, and it must do
 * that rather than crash.
 *
 * Both files go in the same directory on the G5, which is what the plain
 * include expects -- there is no host build of this, since it needs
 * ApplicationServices:
 *
 *   scp probes/clipshim.c src/clipboard_shim.c ppctiger:~/ppc-probes/
 *   ssh ppctiger '/opt/local/libexec/gcc10-bootstrap/bin/gcc -O2 -fno-gcse \
 *       -mcpu=970 ~/ppc-probes/clipshim.c -o ~/ppc-probes/clipshim \
 *       -framework ApplicationServices && ~/ppc-probes/clipshim'
 */
#include "clipboard_shim.c"

#include <stdio.h>
#include <string.h>

int main(void)
{
    char buf[256];
    int ok, ch, n, rc;

    ok = rd_clip_ok();
    printf("rd_clip_ok      : %d (%s)\n", ok,
           ok ? "reachable -- this is the Aqua session"
              : "unreachable -- ssh or screen, as expected");

    /* Every one of these must return its error value rather than fault when the
     * pasteboard is unreachable, because that is the common case. */
    ch = rd_clip_changed();
    printf("rd_clip_changed : %d\n", ch);

    n = rd_clip_get(buf, (int)sizeof buf);
    printf("rd_clip_get     : %d", n);
    if (n > 0)
        printf(" \"%.*s\"", n > 40 ? 40 : n, buf);
    printf("\n");

    rc = rd_clip_set("clipshim probe", 14);
    printf("rd_clip_set     : %d\n", rc);

    /* Arguments a caller should never pass, checked because the shim is the
     * last thing between a protocol message and a CoreFoundation call. */
    printf("rd_clip_get(NULL): %d\n", rd_clip_get(NULL, 16));
    printf("rd_clip_get(cap 0): %d\n", rd_clip_get(buf, 0));
    printf("rd_clip_set(NULL): %d\n", rd_clip_set(NULL, 4));
    printf("rd_clip_set(len -1): %d\n", rd_clip_set("x", -1));

    if (ok) {
        /* In the session where it works, the round trip is the thing. */
        rc = rd_clip_set("clipshim round trip", 19);
        n = rd_clip_get(buf, (int)sizeof buf);
        printf("=> round trip: set %d, got %d, %s\n", rc, n,
               (n == 19 && memcmp(buf, "clipshim round trip", 19) == 0)
                   ? "MATCHES" : "DID NOT MATCH");
        printf("=> changed after our own write: %d\n", rd_clip_changed());
    } else {
        printf("=> failure path only; rerun from Terminal.app on the G5 for the rest\n");
    }
    return 0;
}
