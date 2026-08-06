/* What does PasteboardSynchronize actually report, in the session that has one?
 *
 * The agent polls `kPasteboardModified` to notice that someone copied something
 * on the G5, because reading the whole clipboard several times a second is not
 * free. Against a real pasteboard it never fired: the peer-to-Mac direction
 * works and writes land, but nothing ever came back the other way.
 *
 * That question cannot be asked over ssh -- PasteboardCreate returns -4960 there
 * -- so this runs as a LaunchAgent, which is the one way into the Aqua session
 * from a remote shell. It reports the flags on every tick, along with the text,
 * so the flag's behaviour can be read against what is actually on the clipboard
 * rather than guessed at:
 *
 *   - does the first synchronise in a process report modified?
 *   - does a change made by another application report modified?
 *   - does our own write report modified on the next tick?
 *
 * Install: see deploy/probe-in-aqua.plist. Copy something on the G5 while it
 * runs, and look for the tick where the text changes.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *stamp(void)
{
    static char buf[16];
    time_t t = time(NULL);
    strftime(buf, sizeof buf, "%H:%M:%S", localtime(&t));
    return buf;
}

static int read_text(PasteboardRef pb, char *out, int cap)
{
    ItemCount n = 0, i;
    if (PasteboardGetItemCount(pb, &n) != noErr)
        return -1;
    for (i = 1; i <= n; i++) {
        PasteboardItemID id;
        CFDataRef data = NULL;
        CFIndex len;
        if (PasteboardGetItemIdentifier(pb, i, &id) != noErr)
            continue;
        if (PasteboardCopyItemFlavorData(pb, id, CFSTR("public.utf8-plain-text"),
                                         &data) != noErr || !data)
            continue;
        len = CFDataGetLength(data);
        if (len > cap - 1)
            len = cap - 1;
        CFDataGetBytes(data, CFRangeMake(0, len), (UInt8 *)out);
        out[len] = 0;
        CFRelease(data);
        return (int)len;
    }
    return 0;
}

int main(void)
{
    PasteboardRef pb = NULL;
    OSStatus err = PasteboardCreate(kPasteboardClipboard, &pb);
    char cur[1024], prev[1024];
    int tick;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("%s clipwatch: PasteboardCreate = %ld %s\n", stamp(), (long)err,
           err == noErr ? "(this IS the Aqua session)" : "(NOT reachable)");
    if (err != noErr)
        return 1;

    prev[0] = 0;
    for (tick = 0; tick < 240; tick++) {
        PasteboardSyncFlags f = PasteboardSynchronize(pb);
        int n = read_text(pb, cur, sizeof cur);
        int text_changed = strcmp(cur, prev) != 0;

        /* The two things being compared: what the flag claims, and whether the
         * text is in fact different from last time. If the second happens
         * without the first, polling the flag is the wrong mechanism. */
        printf("%s tick %3d flags=0x%lx%s%s | %d bytes \"%.40s\"%s\n",
               stamp(), tick, (unsigned long)f,
               (f & kPasteboardModified) ? " MODIFIED" : "",
               (f & kPasteboardClientIsOwner) ? " owner" : "",
               n, n > 0 ? cur : "",
               text_changed ? "   <-- TEXT CHANGED" : "");

        if (text_changed && !(f & kPasteboardModified))
            printf("%s   ^^ the text changed and the flag did NOT say so\n", stamp());

        strcpy(prev, cur);
        sleep(2);
    }
    return 0;
}
