/* One-shot framebuffer read: checksum now, and optionally dump the pixels.
 *
 * Run this repeatedly with a forced screen change in between. If each *process*
 * sees different content while a single long-lived process never does, the
 * mapping is a per-process snapshot rather than the live scanout — which is a
 * completely different bug from "the framebuffer is frozen", and has a
 * completely different fix.
 *
 *   fb-shot            checksum only
 *   fb-shot out.ppm    also dump the frame as a binary PPM
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    CGDirectDisplayID d = CGMainDisplayID();
    size_t w = CGDisplayPixelsWide(d), h = CGDisplayPixelsHigh(d);
    size_t bpr = CGDisplayBytesPerRow(d);
    unsigned char *base = (unsigned char *)CGDisplayBaseAddress(d);
    if (!base) {
        printf("base = NULL\n");
        return 1;
    }

    /* Copy out of VRAM in one go; scattered reads across the bus are ruinous. */
    unsigned char *buf = malloc(bpr * h);
    memcpy(buf, base, bpr * h);

    unsigned long sum = 0;
    unsigned long nonblack = 0;
    size_t y, x;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char *p = buf + y * bpr + x * 4;
            sum = sum * 31u + p[1] + p[2] + p[3];
            if (p[1] || p[2] || p[3])
                nonblack++;
        }
    }
    printf("base=%p sum=%lu nonblack=%lu/%lu (%.1f%%) first=%02x %02x %02x %02x\n",
           base, sum, nonblack, (unsigned long)(w * h),
           100.0 * nonblack / (double)(w * h),
           buf[0], buf[1], buf[2], buf[3]);

    if (argc > 1) {
        /* Memory order is A,R,G,B (big-endian ARGB32), so RGB starts at +1. */
        FILE *f = fopen(argv[1], "wb");
        fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                fwrite(buf + y * bpr + x * 4 + 1, 1, 3, f);
        fclose(f);
        printf("wrote %s\n", argv[1]);
    }
    free(buf);
    return 0;
}
