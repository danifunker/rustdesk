/* VP8-lite: a small VP8 encoder for screens, written for a 68040.
 *
 * A stock RustDesk client decodes VP8, VP9, AV1 and H.26x and nothing else, so
 * the agent has to speak one of them. This speaks VP8 while using as little of
 * it as it can get away with:
 *
 *   - An unchanged macroblock is an inter macroblock with a zero motion vector
 *     and no coefficients. It costs a fraction of a bit and no arithmetic.
 *   - A changed one is predicted either from the same place in the last frame
 *     or from its neighbours (DC, V, H or TM over the whole 16x16), whichever
 *     is closer, and the difference is coded. Flat areas come out of DC
 *     prediction with nothing left to code.
 *   - One quantiser, default probabilities, no segmentation, one token
 *     partition, and the loop filter off. With no loop filter the encoder's
 *     reconstruction is bit-exact with what the peer decodes, so a macroblock
 *     that is left alone stays exactly as the peer last saw it.
 *
 * Everything is plain integer C with no allocation after vp8e_init, so the same
 * file builds for the Mac and for the Linux tests that check it against libvpx.
 */
#ifndef VP8ENC_H
#define VP8ENC_H

#include <stddef.h>
#include <stdint.h>

typedef struct vp8e vp8e;

/* The planes the encoder reads from: I420, the caller's memory, the whole frame.
 * Only macroblocks marked dirty are read on an inter frame. */
typedef struct {
    const uint8_t *y, *u, *v;
    int ystride, uvstride;
} vp8e_src;

/* Bytes of working memory vp8e_init needs for a w x h frame. */
size_t vp8e_mem_size(int w, int h);

/* Set up an encoder in `mem` (vp8e_mem_size bytes, 4-byte aligned). `q` is the
 * VP8 quantiser index, 0 (best) to 127. Returns NULL if w or h is unusable. */
vp8e *vp8e_init(void *mem, int w, int h, int q);

void vp8e_set_q(vp8e *e, int q);
int vp8e_mb_cols(const vp8e *e);
int vp8e_mb_rows(const vp8e *e);

/* Encode one frame into out[0..cap). `dirty` has one byte per macroblock,
 * row-major, nonzero where the source changed; NULL means all of it. A
 * keyframe ignores `dirty` and codes everything. Returns the frame's length,
 * or 0 if it did not fit (the encoder state is then as if nothing was sent,
 * and the next frame must be a keyframe). */
size_t vp8e_encode(vp8e *e, const vp8e_src *src, const uint8_t *dirty, int key,
                   uint8_t *out, size_t cap);

/* The same, a few macroblock rows at a time, for a caller that cannot hold
 * the processor for a whole frame. vp8e_begin returns 0 if `cap` is too small;
 * vp8e_rows returns nonzero once every row is done; vp8e_end then assembles
 * the frame and returns its length (0: it did not fit). The source planes and
 * dirty map must not change until vp8e_end. A frame that is begun and never
 * ended must be vp8e_abandon'ed: the next one is then a keyframe. */
int vp8e_begin(vp8e *e, const vp8e_src *src, const uint8_t *dirty, int key, uint8_t *out,
               size_t cap);
int vp8e_rows(vp8e *e, int nrows);
size_t vp8e_end(vp8e *e);
void vp8e_abandon(vp8e *e);

/* The encoder's reconstruction: exactly what a decoder shows after the last
 * frame. Tests compare it against libvpx. */
void vp8e_recon(const vp8e *e, const uint8_t **y, const uint8_t **u,
                const uint8_t **v, int *ystride, int *uvstride);

/* Counters from the last vp8e_encode, for logs and tuning. */
typedef struct {
    int mbs, skipped, inter, intra;
    int key;   /* the frame was a keyframe, asked for or not */
} vp8e_stats;
void vp8e_last_stats(const vp8e *e, vp8e_stats *s);

#endif
