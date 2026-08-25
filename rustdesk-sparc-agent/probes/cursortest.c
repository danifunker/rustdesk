/*
 * cursortest.c -- is the shape XFIXES hands back actually a pointer?
 *
 * Prints the size, hotspot and serial, then draws the alpha channel as text.
 * A cursor read wrongly does not look wrong in a hex dump, but it is
 * unmistakable as a picture -- and the failure this guards against, reading
 * `unsigned long` pixels as packed u32, produces exactly half an arrow.
 *
 *   gcc -O1 -o cursortest cursortest.c ../src/cursor_shim.c -lXfixes -lX11
 *
 *   gcc-5.5 -m64 -O2 -I/usr/openwin/include -o cursortest cursortest.c \
 *       ../src/cursor_shim.c -L/usr/openwin/lib/sparcv9 \
 *       -R/usr/openwin/lib/sparcv9 -L/usr/openwin/sfw/lib/sparcv9 \
 *       -R/usr/openwin/sfw/lib/sparcv9 -lXfixes -lX11
 */

#include <stdio.h>

int rd_cursor_seed(void);
int rd_cursor_image(unsigned char *out, int out_len, int *w, int *h,
                    int *hotx, int *hoty);

int main(void)
{
    static unsigned char rgba[128 * 128 * 4];
    int w = 0, h = 0, hotx = 0, hoty = 0, n, x, y;

    printf("seed %d\n", rd_cursor_seed());
    n = rd_cursor_image(rgba, (int)sizeof rgba, &w, &h, &hotx, &hoty);
    if (n <= 0) {
        printf("no cursor image (%d)\n", n);
        return 1;
    }
    printf("%dx%d hotspot %d,%d, %d bytes\n", w, h, hotx, hoty, n);

    for (y = 0; y < h; y++) {
        printf("  ");
        for (x = 0; x < w; x++) {
            unsigned char a = rgba[(y * w + x) * 4 + 3];
            unsigned char r = rgba[(y * w + x) * 4 + 0];
            /* '#' opaque and dark, '.' opaque and light, ':' partly there. */
            putchar(a < 32 ? ' ' : (a < 200 ? ':' : (r < 128 ? '#' : '.')));
        }
        putchar('\n');
    }
    return 0;
}
