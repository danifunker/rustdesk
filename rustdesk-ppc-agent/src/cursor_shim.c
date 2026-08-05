/* The system cursor image, which is not otherwise reachable.
 *
 * `NSCursor` knows only the calling application's own cursor, and the pointer
 * is a hardware overlay so it is not in the framebuffer either -- a 100x100
 * patch of a captured frame centred on it holds exactly one colour. The window
 * server keeps the global one, and the only way to it on this vintage is the
 * private CGS calls below. They are what every pre-10.6 VNC server used for
 * exactly this, and they are verified working on 10.5.8:
 *
 *     connection id : 52387        rect     : 0,0 24x24
 *     cursor seed   : 1442         hotspot  : 4,4
 *     data size     : 2304         depth 32, components 4, bits/component 8
 *
 * Private means undocumented, not unstable: the signatures have been the same
 * since 10.4. Every call is still checked, and a failure degrades to the
 * built-in arrow rather than to no pointer at all -- see `crate::cursor`.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdlib.h>
#include <string.h>

typedef int CGSConnectionID;

extern CGSConnectionID CGSMainConnectionID(void);
extern CGError CGSGetGlobalCursorDataSize(CGSConnectionID cid, int *size);
extern CGError CGSGetGlobalCursorData(CGSConnectionID cid, unsigned char *data,
                                      int *size, int *bytes_per_row, CGRect *rect,
                                      CGPoint *hotspot, int *depth, int *components,
                                      int *bits_per_component);
extern int CGSCurrentCursorSeed(void);

/* Bumped by the window server whenever the pointer changes shape.
 *
 * The point of it is that it is cheap: polling this every pass costs one call,
 * where fetching the image costs an allocation and a copy. */
int rd_cursor_seed(void)
{
    return CGSCurrentCursorSeed();
}

/* Copy the current cursor into `out` as RGBA, returning the bytes written.
 *
 * Returns -1 if anything is unavailable or unexpected, including a cursor that
 * does not fit: the caller keeps whatever shape it had rather than sending a
 * truncated one.
 *
 * The window server hands the image over in A,R,G,B memory order -- the same
 * big-endian layout as the framebuffer, established by dumping a real cursor
 * and finding byte 0 non-zero across the whole arrow silhouette while bytes 1
 * to 3 were non-zero only over its white interior. `CursorData` wants RGBA, so
 * the channels rotate on the way out.
 */
int rd_cursor_image(unsigned char *out, int out_len, int *w, int *h,
                    int *hotx, int *hoty)
{
    CGSConnectionID cid;
    unsigned char *buf;
    int size = 0, bpr = 0, depth = 0, comps = 0, bpc = 0;
    CGRect rect;
    CGPoint hot;
    int width, height, x, y, written;

    if (!out || !w || !h || !hotx || !hoty || out_len <= 0)
        return -1;

    cid = CGSMainConnectionID();
    if (CGSGetGlobalCursorDataSize(cid, &size) != kCGErrorSuccess || size <= 0)
        return -1;

    buf = malloc(size);
    if (!buf)
        return -1;

    if (CGSGetGlobalCursorData(cid, buf, &size, &bpr, &rect, &hot,
                               &depth, &comps, &bpc) != kCGErrorSuccess) {
        free(buf);
        return -1;
    }

    /* Only the one layout is handled. A cursor arriving as anything else is
     * better skipped than reinterpreted into confetti. */
    if (depth != 32 || comps != 4 || bpc != 8) {
        free(buf);
        return -1;
    }

    width = (int)rect.size.width;
    height = (int)rect.size.height;
    if (width <= 0 || height <= 0 || bpr < width * 4 || size < bpr * height) {
        free(buf);
        return -1;
    }
    written = width * height * 4;
    if (written > out_len) {
        free(buf);
        return -1;
    }

    for (y = 0; y < height; y++) {
        const unsigned char *src = buf + (size_t)y * bpr;
        unsigned char *dst = out + (size_t)y * width * 4;
        for (x = 0; x < width; x++) {
            unsigned char a = src[x * 4 + 0];
            unsigned char r = src[x * 4 + 1];
            unsigned char g = src[x * 4 + 2];
            unsigned char b = src[x * 4 + 3];
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }

    *w = width;
    *h = height;
    *hotx = (int)hot.x;
    *hoty = (int)hot.y;
    free(buf);
    return written;
}
