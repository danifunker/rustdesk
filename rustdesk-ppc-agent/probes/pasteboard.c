/* Is the clipboard reachable from where the agent actually runs?
 *
 * Everything else in backlog item 6 depends on this and nothing else answers
 * it. The agent runs under a detached `screen`, which keeps enough of the login
 * session that CGDisplayBaseAddress works -- but the pasteboard is a different
 * server, and item 9 records that a *fully* detached agent cannot reach the
 * window server at all. So the same question has to be asked again here rather
 * than assumed from the fact that capture works.
 *
 * Reads the clipboard, writes a known string, reads it back, and reports the
 * change count each time. Run it over ssh, and run it again from the agent's
 * own screen session; if the two disagree, the answer is the session and not
 * the API.
 *
 *   PPC_HOST=ppctiger probes/run.sh pasteboard
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>

/* Read the first UTF-8 text flavour on the clipboard into `out`.
 * Returns its length, -1 if there is no text, or -2 if the pasteboard itself
 * could not be reached. */
static int read_text(PasteboardRef pb, char *out, int cap)
{
    ItemCount n = 0, i;
    if (!pb)
        return -2;
    PasteboardSynchronize(pb);
    if (PasteboardGetItemCount(pb, &n) != noErr)
        return -2;
    for (i = 1; i <= n; i++) {
        PasteboardItemID id;
        CFDataRef data = NULL;
        if (PasteboardGetItemIdentifier(pb, i, &id) != noErr)
            continue;
        if (PasteboardCopyItemFlavorData(pb, id, CFSTR("public.utf8-plain-text"),
                                         &data) == noErr && data) {
            CFIndex len = CFDataGetLength(data);
            if (len > cap - 1)
                len = cap - 1;
            CFDataGetBytes(data, CFRangeMake(0, len), (UInt8 *)out);
            out[len] = 0;
            CFRelease(data);
            return (int)len;
        }
        if (data)
            CFRelease(data);
    }
    return -1;
}

static OSStatus write_text(PasteboardRef pb, const char *s)
{
    CFDataRef data;
    OSStatus err;
    if (!pb)
        return -1;
    if ((err = PasteboardClear(pb)) != noErr)
        return err;
    PasteboardSynchronize(pb);
    data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)s, (CFIndex)strlen(s));
    if (!data)
        return -1;
    err = PasteboardPutItemFlavor(pb, (PasteboardItemID)1,
                                  CFSTR("public.utf8-plain-text"), data, 0);
    CFRelease(data);
    return err;
}

int main(void)
{
    PasteboardRef pb = NULL;
    OSStatus err = PasteboardCreate(kPasteboardClipboard, &pb);
    char buf[4096];
    int n;

    printf("PasteboardCreate: %ld %s\n", (long)err, err == noErr ? "ok" : "FAILED");
    if (err != noErr || !pb) {
        printf("=> the clipboard is NOT reachable from this session\n");
        return 1;
    }

    /* What the sync flags say is how a poll would notice someone else copying:
     * kPasteboardModified is set when the contents changed since the last
     * synchronise, which is the cheap change check the agent would need. */
    PasteboardSyncFlags f = PasteboardSynchronize(pb);
    printf("sync flags: 0x%lx%s%s\n", (unsigned long)f,
           (f & kPasteboardModified) ? " modified" : "",
           (f & kPasteboardClientIsOwner) ? " we-own-it" : "");

    n = read_text(pb, buf, sizeof buf);
    if (n >= 0)
        printf("read  : %d bytes, \"%.60s\"%s\n", n, buf, n > 60 ? "..." : "");
    else
        printf("read  : %s\n", n == -1 ? "no text flavour on the clipboard" : "UNREACHABLE");

    err = write_text(pb, "rustdesk-ppc-agent pasteboard probe");
    printf("write : %ld %s\n", (long)err, err == noErr ? "ok" : "FAILED");

    n = read_text(pb, buf, sizeof buf);
    if (n >= 0 && strcmp(buf, "rustdesk-ppc-agent pasteboard probe") == 0)
        printf("=> round trip WORKS: the clipboard is usable from here\n");
    else
        printf("=> round trip FAILED: read back %d bytes, \"%s\"\n", n, n >= 0 ? buf : "");

    /* Whether a second synchronise reports our own write as a modification
     * decides whether a naive poll would send the peer its own clipboard back
     * in a loop. */
    f = PasteboardSynchronize(pb);
    printf("after our write, sync flags: 0x%lx%s%s\n", (unsigned long)f,
           (f & kPasteboardModified) ? " modified" : "",
           (f & kPasteboardClientIsOwner) ? " we-own-it" : "");
    return 0;
}
