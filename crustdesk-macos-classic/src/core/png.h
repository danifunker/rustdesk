/* PNG for screenshots (ScreenshotResponse), with no zlib: deflate written
 * here, LZ77 over a 32 KB window with fixed Huffman codes -- a desktop is
 * mostly flat fills and repeated rows, which that alone takes most of.
 *
 * Streaming: the caller hands rows over one at a time (indices for a
 * palette image, R G B otherwise), so the only copy of the picture is the
 * compressed one. The encoder's state is ~150 KB; allocate it, do not put it
 * on a Mac's stack.
 */
#ifndef CDV_PNG_H
#define CDV_PNG_H

#include <stddef.h>
#include <stdint.h>

typedef struct png_writer png_writer;
size_t png_writer_size(void);

/* Start a PNG into out[0..cap). `palette` (npal entries of R, G, B) for an
 * 8-bit indexed image, NULL for 24-bit RGB. */
void png_begin(png_writer *p, uint8_t *out, size_t cap, int width, int height,
               const uint8_t (*palette)[3], int npal);
/* One row: width bytes (indexed) or width * 3 (RGB), top to bottom. */
void png_row(png_writer *p, const uint8_t *row);
/* The PNG's length, or 0 if it did not fit in cap. */
size_t png_end(png_writer *p);

#endif
