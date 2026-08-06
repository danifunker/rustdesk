/* The Mac clipboard, as UTF-8 text.
 *
 * In C for the same reason as the other shims: the Pasteboard Manager is a
 * CoreFoundation API, and CFDataRef/CFStringRef lifetimes are much less
 * ceremony here than through an FFI.
 *
 * **This needs the Aqua session and does not degrade quietly.** Measured with
 * probes/pasteboard.c: `PasteboardCreate` returns -4960 from an ssh login *and*
 * from the detached `screen` the agent normally runs under, and `pbcopy` and
 * `pbpaste` fail there too -- so it is the session and not the API. Capture is
 * not a guide here: CGDisplayBaseAddress works fine over ssh, which is why the
 * question had to be asked separately. The agent must be started from the
 * LaunchAgent in deploy/ for any of this to do anything, and `rd_clip_ok`
 * exists so it can say so once rather than fail silently for ever.
 *
 * Text only, and `public.utf8-plain-text` only: see src/clipboard.rs.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdlib.h>
#include <string.h>

static PasteboardRef g_pb;
static int g_tried;

/* The pasteboard reference, created once.
 *
 * Retried on every call while it is failing rather than cached as dead: the
 * agent may outlive a logout and login, and a session that appears later
 * should start working without a restart. That is the same mistake item 1d
 * records for video -- giving up permanently on one failure.
 */
static PasteboardRef pb(void)
{
    if (!g_pb) {
        if (PasteboardCreate(kPasteboardClipboard, &g_pb) != noErr)
            g_pb = NULL;
        g_tried = 1;
    }
    return g_pb;
}

/* Is the clipboard reachable at all? Non-zero if so. */
int rd_clip_ok(void)
{
    return pb() != NULL;
}

/* The clipboard's change count.
 *
 * `PasteboardSynchronize` reports kPasteboardModified when the contents have
 * changed since the last call, which is the cheap poll the session loop needs:
 * reading the text every pass would mean copying the whole clipboard several
 * times a second.
 *
 * Returns 1 if it changed, 0 if not, -1 if the pasteboard is unreachable.
 * kPasteboardClientIsOwner deliberately does not suppress the change: we own it
 * precisely when we were the ones who wrote it, and the caller has its own,
 * better test for that -- comparing the text.
 */
int rd_clip_changed(void)
{
    PasteboardRef p = pb();
    PasteboardSyncFlags f;
    if (!p)
        return -1;
    f = PasteboardSynchronize(p);
    return (f & kPasteboardModified) ? 1 : 0;
}

/* Copy the clipboard's UTF-8 text into `out`.
 *
 * Returns the number of bytes written, 0 if there is no text on the clipboard,
 * or -1 if the pasteboard is unreachable. A text longer than `cap` is refused
 * with -2 rather than truncated: half a paste is worse than none, and cutting
 * UTF-8 at a byte boundary can split a character.
 */
int rd_clip_get(char *out, int cap)
{
    PasteboardRef p = pb();
    ItemCount n = 0, i;

    if (!p || !out || cap <= 0)
        return -1;
    PasteboardSynchronize(p);
    if (PasteboardGetItemCount(p, &n) != noErr)
        return -1;

    for (i = 1; i <= n; i++) {
        PasteboardItemID id;
        CFDataRef data = NULL;
        CFIndex len;

        if (PasteboardGetItemIdentifier(p, i, &id) != noErr)
            continue;
        if (PasteboardCopyItemFlavorData(p, id, CFSTR("public.utf8-plain-text"),
                                         &data) != noErr || !data)
            continue;

        len = CFDataGetLength(data);
        if (len > cap) {
            CFRelease(data);
            return -2;
        }
        CFDataGetBytes(data, CFRangeMake(0, len), (UInt8 *)out);
        CFRelease(data);
        return (int)len;
    }
    return 0;
}

/* Put UTF-8 text on the clipboard. Returns 0 on success, -1 otherwise.
 *
 * `len` is passed rather than relying on a terminator: a clipboard may
 * legitimately contain a NUL, and finding out by truncating there would be a
 * silent corruption.
 */
int rd_clip_set(const char *text, int len)
{
    PasteboardRef p = pb();
    CFDataRef data;
    OSStatus err;

    if (!p || !text || len < 0)
        return -1;
    if (PasteboardClear(p) != noErr)
        return -1;
    /* Clearing makes us the owner; synchronising here is what makes that
     * visible to the next change check rather than at some later point. */
    PasteboardSynchronize(p);

    data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)text, (CFIndex)len);
    if (!data)
        return -1;
    err = PasteboardPutItemFlavor(p, (PasteboardItemID)1,
                                  CFSTR("public.utf8-plain-text"), data, 0);
    CFRelease(data);
    return err == noErr ? 0 : -1;
}
