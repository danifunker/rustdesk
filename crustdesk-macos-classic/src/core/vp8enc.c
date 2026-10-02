/* VP8-lite encoder. See vp8enc.h for what it does and does not use.
 *
 * Section numbers refer to RFC 6386. The tables in vp8tab.h are generated from
 * that document by tools/gen_vp8_tables.py.
 */
#include "vp8enc.h"
#include "vp8tab.h"

#include <string.h>

enum { DC_PRED = 0, V_PRED = 1, H_PRED = 2, TM_PRED = 3, B_PRED = 4 };

/* Copy n bytes (8 or 16) of a block row. libc's memcpy for 16 bytes is a
 * call and an alignment dance; macroblock rows are word-aligned whenever
 * the planes are, so copy words then. */
static inline __attribute__((always_inline)) void copy_row(uint8_t *d, const uint8_t *s, int n)
{
    if (!(((unsigned long)d | (unsigned long)s) & 3)) {
        uint32_t *dw = (uint32_t *)d;
        const uint32_t *sw = (const uint32_t *)s;
        dw[0] = sw[0];
        dw[1] = sw[1];
        if (n == 16) {
            dw[2] = sw[2];
            dw[3] = sw[3];
        }
    } else {
        memcpy(d, s, (size_t)n);
    }
}


/* Where the time goes, by phase, when built with VP8E_PROFILE and a platform
 * clock: 0 mode search, 1 transform and quantise, 2 reconstruct, 3 the
 * exactness check, 4 tokens. */
#ifdef VP8E_PROFILE
uint32_t vp8e_clock(void);
#define PROF_START uint32_t prof_t = vp8e_clock()
#define PROF(slot)                                                                             \
    do {                                                                                       \
        uint32_t prof_n = vp8e_clock();                                                        \
        e->prof[slot] += prof_n - prof_t;                                                      \
        prof_t = prof_n;                                                                       \
    } while (0)
#else
#define PROF_START
#define PROF(slot)
#endif

/* Per-macroblock record kept for the whole frame, because the first partition
 * (modes) is written after every macroblock has been decided. */
#define MB_INTER 0x01
#define MB_SKIP  0x80
#define MB_YMODE(i)   (((i) >> 1) & 7)
#define MB_UVMODE(i)  (((i) >> 4) & 7)


/* ---- boolean entropy encoder, section 7.3 ------------------------------- */

typedef struct {
    uint8_t *out, *start, *end;
    uint32_t range, low;
    int count;
    int overflow;
} bw;

/* How far to shift a range to bring it back to 128 or more. */
static uint8_t norm[256];

static void norm_init(void)
{
    int r;
    if (norm[1])
        return;
    for (r = 1; r < 256; r++) {
        int n = 0;
        while ((r << n) < 128)
            n++;
        norm[r] = (uint8_t)n;
    }
}

static void bw_init(bw *b, uint8_t *start, uint8_t *end)
{
    b->out = b->start = start;
    b->end = end;
    b->range = 255;
    b->low = 0;
    b->count = -24;
    b->overflow = 0;
}

/* One bool, section 7.3, in libvpx's form: the renormalising shift comes from
 * a table instead of a loop, and a byte is written once 8 bits are ready.
 * This is the encoder's innermost call -- hundreds of times a macroblock. */
static inline __attribute__((always_inline)) void bw_put(bw *b, int prob, int bit)
{
    uint32_t split = 1 + (((b->range - 1) * (uint32_t)prob) >> 8);
    uint32_t range = split, low = b->low;
    int shift, count = b->count;
    if (bit) {
        low += split;
        range = b->range - split;
    }
    shift = norm[range];
    range <<= shift;
    count += shift;
    if (count >= 0) {
        int offset = shift - count;
        if ((low << (offset - 1)) & 0x80000000u) {
            uint8_t *q = b->out;
            while (q > b->start && *--q == 255)
                *q = 0;
            ++*q;
        }
        if (b->out < b->end)
            *b->out++ = (uint8_t)(low >> (24 - offset));
        else
            b->overflow = 1;
        low <<= offset;
        shift = count;
        low &= 0xffffff;
        count -= 8;
    }
    b->low = low << shift;
    b->count = count;
    b->range = range;
}

static void bw_literal(bw *b, uint32_t v, int bits)
{
    while (bits-- > 0)
        bw_put(b, 128, (v >> bits) & 1);
}

/* libvpx ends a partition with 32 zero bits at even odds, which pushes every
 * pending bit out as whole bytes. */
static size_t bw_finish(bw *b)
{
    int i;
    for (i = 0; i < 32; i++)
        bw_put(b, 128, 0);
    return (size_t)(b->out - b->start);
}

/* Find the path to leaf `v` (leaves are stored negated, section 8.1). */
static int tree_find(const signed char *t, int i, int v, int *nodes, int *bits, int depth)
{
    int k;
    for (k = 0; k < 2; k++) {
        int x = t[i + k];
        nodes[depth] = i;
        bits[depth] = k;
        if (x <= 0) {
            if (-x == v)
                return depth + 1;
        } else {
            int d = tree_find(t, x, v, nodes, bits, depth + 1);
            if (d)
                return d;
        }
    }
    return 0;
}

static void bw_tree(bw *b, const signed char *tree, const uint8_t *probs, int value)
{
    int nodes[8], bits[8], n = tree_find(tree, 0, value, nodes, bits, 0), j;
    for (j = 0; j < n; j++)
        bw_put(b, probs[nodes[j] >> 1], bits[j]);
}

/* Trees, section 8.1 and 11.2-11.4. */
static const signed char kf_ymode_tree[8] = { -B_PRED, 2, 4, 6, -DC_PRED, -V_PRED, -H_PRED, -TM_PRED };
static const signed char ymode_tree[8] = { -DC_PRED, 2, 4, 6, -V_PRED, -H_PRED, -TM_PRED, -B_PRED };
static const signed char uv_mode_tree[6] = { -DC_PRED, 2, -V_PRED, 4, -H_PRED, -TM_PRED };
static const uint8_t kf_ymode_prob[4] = { 145, 156, 163, 128 };
static const uint8_t ymode_prob[4] = { 112, 86, 140, 37 };
static const uint8_t kf_uv_mode_prob[3] = { 142, 114, 183 };
static const uint8_t uv_mode_prob[3] = { 162, 101, 204 };

/* ---- tokens, section 13 --------------------------------------------------- */

static const uint8_t zigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t coef_bands[16] = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7 };
static const uint8_t pcat1[] = { 159 };
static const uint8_t pcat2[] = { 165, 145 };
static const uint8_t pcat3[] = { 173, 148, 140 };
static const uint8_t pcat4[] = { 176, 155, 140, 135 };
static const uint8_t pcat5[] = { 180, 157, 141, 134, 130 };
static const uint8_t pcat6[] = { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129 };

static inline __attribute__((always_inline)) void put_extra(bw *b, const uint8_t *p, int bits,
                                                              int v)
{
    int i;
    for (i = 0; i < bits; i++)
        bw_put(b, p[i], (v >> (bits - 1 - i)) & 1);
}

/* Code one block's tokens. `qc` is in zigzag order. Returns whether anything
 * nonzero was coded, which is the context its neighbours see. */
static int put_block(bw *bp, const short *qc, int type, int first, int ctx)
{
    /* The coder's state in a local, so it can live in registers for the
     * block instead of going through memory on every bool. */
    bw local = *bp, *b = &local;
    int last = -1, c, skip_eob = 0;
    for (c = 15; c >= first; c--)
        if (qc[c]) {
            last = c;
            break;
        }
    for (c = first; c < 16; c++) {
        const uint8_t *p = vp8_default_coef_probs[type][coef_bands[c]][ctx];
        int v = qc[c], a;
        if (!skip_eob) {
            if (c > last) {
                bw_put(b, p[0], 0);
                break;
            }
            bw_put(b, p[0], 1);
        }
        if (v == 0) {
            bw_put(b, p[1], 0);
            ctx = 0;
            skip_eob = 1;
            continue;
        }
        bw_put(b, p[1], 1);
        a = v < 0 ? -v : v;
        if (a == 1) {
            bw_put(b, p[2], 0);
            ctx = 1;
        } else {
            bw_put(b, p[2], 1);
            ctx = 2;
            if (a <= 4) {
                bw_put(b, p[3], 0);
                if (a == 2) {
                    bw_put(b, p[4], 0);
                } else {
                    bw_put(b, p[4], 1);
                    bw_put(b, p[5], a == 4);
                }
            } else if (a <= 10) {
                bw_put(b, p[3], 1);
                bw_put(b, p[6], 0);
                if (a <= 6) {
                    bw_put(b, p[7], 0);
                    put_extra(b, pcat1, 1, a - 5);
                } else {
                    bw_put(b, p[7], 1);
                    put_extra(b, pcat2, 2, a - 7);
                }
            } else {
                bw_put(b, p[3], 1);
                bw_put(b, p[6], 1);
                if (a <= 34) {
                    bw_put(b, p[8], 0);
                    if (a <= 18) {
                        bw_put(b, p[9], 0);
                        put_extra(b, pcat3, 3, a - 11);
                    } else {
                        bw_put(b, p[9], 1);
                        put_extra(b, pcat4, 4, a - 19);
                    }
                } else {
                    bw_put(b, p[8], 1);
                    if (a <= 66) {
                        bw_put(b, p[10], 0);
                        put_extra(b, pcat5, 5, a - 35);
                    } else {
                        bw_put(b, p[10], 1);
                        put_extra(b, pcat6, 11, a - 67);
                    }
                }
            }
        }
        bw_put(b, 128, v < 0);
        skip_eob = 0;
    }
    *bp = local;
    return last >= first;
}

/* ---- transforms ----------------------------------------------------------- */

/* Forward DCT: libvpx's vp8_short_fdct4x4_c, which pairs with the IDCT below.
 * It need not be exact -- only the inverse has to match the decoder. */
static void fdct4x4(const short *in, short *out)
{
    int i, a1, b1, c1, d1;
    const short *ip = in;
    short *op = out;
    for (i = 0; i < 4; i++) {
        a1 = (ip[0] + ip[3]) * 8;
        b1 = (ip[1] + ip[2]) * 8;
        c1 = (ip[1] - ip[2]) * 8;
        d1 = (ip[0] - ip[3]) * 8;
        op[0] = (short)(a1 + b1);
        op[2] = (short)(a1 - b1);
        op[1] = (short)((c1 * 2217 + d1 * 5352 + 14500) >> 12);
        op[3] = (short)((d1 * 2217 - c1 * 5352 + 7500) >> 12);
        ip += 4;
        op += 4;
    }
    ip = out;
    op = out;
    for (i = 0; i < 4; i++) {
        a1 = ip[0] + ip[12];
        b1 = ip[4] + ip[8];
        c1 = ip[4] - ip[8];
        d1 = ip[0] - ip[12];
        op[0] = (short)((a1 + b1 + 7) >> 4);
        op[8] = (short)((a1 - b1 + 7) >> 4);
        op[4] = (short)(((c1 * 2217 + d1 * 5352 + 12000) >> 16) + (d1 != 0));
        op[12] = (short)((d1 * 2217 - c1 * 5352 + 51000) >> 16);
        ip++;
        op++;
    }
}

/* Inverse DCT and add, section 14.3 -- must match the decoder exactly. */
static uint8_t clamp255(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* A block with only a DC coefficient: the full transform below adds
 * (dc + 4) >> 3 to every pixel (libvpx's vp8_dc_only_idct_add). With none,
 * the prediction stands. */
static void idct_dc_add(int dc, const uint8_t *pred, int ps, uint8_t *dst, int ds)
{
    int i, j, a = (dc + 4) >> 3;
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++)
            dst[i] = a ? clamp255(pred[i] + a) : pred[i];
        pred += ps;
        dst += ds;
    }
}

static void idct_add(const short *in, const uint8_t *pred, int ps, uint8_t *dst, int ds)
{
    int i, a1, b1, c1, d1, t1, t2;
    int tmp[16];
    const short *ip = in;
    for (i = 0; i < 4; i++) {
        a1 = ip[0] + ip[8];
        b1 = ip[0] - ip[8];
        t1 = (ip[4] * 35468) >> 16;
        t2 = ip[12] + ((ip[12] * 20091) >> 16);
        c1 = t1 - t2;
        t1 = ip[4] + ((ip[4] * 20091) >> 16);
        t2 = (ip[12] * 35468) >> 16;
        d1 = t1 + t2;
        /* the reference keeps these in shorts */
        tmp[i] = (short)(a1 + d1);
        tmp[12 + i] = (short)(a1 - d1);
        tmp[4 + i] = (short)(b1 + c1);
        tmp[8 + i] = (short)(b1 - c1);
        ip++;
    }
    for (i = 0; i < 4; i++) {
        const int *r = tmp + 4 * i;
        a1 = r[0] + r[2];
        b1 = r[0] - r[2];
        t1 = (r[1] * 35468) >> 16;
        t2 = r[3] + ((r[3] * 20091) >> 16);
        c1 = t1 - t2;
        t1 = r[1] + ((r[1] * 20091) >> 16);
        t2 = (r[3] * 35468) >> 16;
        d1 = t1 + t2;
        dst[0] = clamp255(pred[0] + ((a1 + d1 + 4) >> 3));
        dst[3] = clamp255(pred[3] + ((a1 - d1 + 4) >> 3));
        dst[1] = clamp255(pred[1] + ((b1 + c1 + 4) >> 3));
        dst[2] = clamp255(pred[2] + ((b1 - c1 + 4) >> 3));
        pred += ps;
        dst += ds;
    }
}

/* Inverse Walsh-Hadamard, section 14.3 -- exact. */
static void iwalsh(const short *in, short *out)
{
    int i, a1, b1, c1, d1, a2, b2, c2, d2;
    int t[16];
    for (i = 0; i < 4; i++) {
        a1 = in[i] + in[12 + i];
        b1 = in[4 + i] + in[8 + i];
        c1 = in[4 + i] - in[8 + i];
        d1 = in[i] - in[12 + i];
        t[i] = (short)(a1 + b1);
        t[4 + i] = (short)(c1 + d1);
        t[8 + i] = (short)(a1 - b1);
        t[12 + i] = (short)(d1 - c1);
    }
    for (i = 0; i < 4; i++) {
        const int *r = t + 4 * i;
        a1 = r[0] + r[3];
        b1 = r[1] + r[2];
        c1 = r[1] - r[2];
        d1 = r[0] - r[3];
        a2 = a1 + b1;
        b2 = c1 + d1;
        c2 = a1 - b1;
        d2 = d1 - c1;
        out[4 * i + 0] = (short)((a2 + 3) >> 3);
        out[4 * i + 1] = (short)((b2 + 3) >> 3);
        out[4 * i + 2] = (short)((c2 + 3) >> 3);
        out[4 * i + 3] = (short)((d2 + 3) >> 3);
    }
}

/* Forward Walsh-Hadamard as the algebraic inverse of iwalsh: the inverse is
 * H C H^T / 8 with H H^T = 4I, so C = H^T D H / 2 undoes it. */
static void fwalsh(const short *in, short *out)
{
    static const signed char H[4][4] = {
        { 1, 1, 1, 1 }, { 1, 1, -1, -1 }, { 1, -1, -1, 1 }, { 1, -1, 1, -1 }
    };
    int t[4][4], i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            int s = 0;
            for (k = 0; k < 4; k++)
                s += H[k][i] * in[4 * k + j];
            t[i][j] = s;
        }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            int s = 0;
            for (k = 0; k < 4; k++)
                s += t[i][k] * H[k][j];
            out[4 * i + j] = (short)(s >= 0 ? (s + 1) >> 1 : -((-s + 1) >> 1));
        }
}

/* A quantiser step with its reciprocal: a 68040 divides in 44 cycles and
 * this runs for every coefficient. Most coefficients of screen content
 * quantise to zero, which is one compare. */
typedef struct {
    int step, half;
    uint32_t recip; /* ceil(65536 / step) */
} qstep;

static void qstep_set(qstep *q, int step)
{
    q->step = step;
    q->half = step >> 1;
    q->recip = (65536u + (uint32_t)step - 1) / (uint32_t)step;
}

static short quant(int x, const qstep *qs)
{
    int a = x < 0 ? -x : x, q;
    if (a < qs->step) /* (a + half) / step is 0 or 1 */
        return (short)(a < qs->step - qs->half ? 0 : (x < 0 ? -1 : 1));
    q = (int)(((uint32_t)(a + qs->half) * qs->recip) >> 16);
    if (q > 2048 + 66)
        q = 2048 + 66;
    return (short)(x < 0 ? -q : q);
}

typedef struct {
    short y2[16];
    short y[16][16];
    short uv[8][16];
} mbcoef;

/* A keyframe macroblock's whole outcome is fixed by its source pixels and the
 * reconstructed edges its intra predictors read. When both match one coded a
 * moment ago -- a dithered desktop is the same 16x16 block a thousand times,
 * a window fill the same flat one -- the mode, the coefficients and the
 * reconstruction are the same, and only the tokens need writing again. */
#define IC_ENTRIES 4
#define IC_KEY (384 + 1 + 17 + 16 + 2 * (9 + 8))
typedef struct {
    int valid;
    uint8_t key[IC_KEY];
    uint8_t mode, uvmode, coded, exact;
    mbcoef qc;
    uint8_t ry[256], ru[64], rv[64];
} icache_entry;

struct vp8e {
    int w, h, mbw, mbh, q;
    int have_ref;
    uint8_t *ry, *ru, *rv;        /* reconstruction, macroblock aligned */
    int rys, ruvs;
    uint8_t *mbinfo;              /* mbw * mbh */
    int32_t *mvs;                 /* mbw * mbh: each macroblock's vector (mv_pack), 0 if none */
    uint8_t *refy, *refu, *refv;  /* the previous frame, where vectors read (vp8e_set_ref) */
    const vp8e_hint *hints;       /* this frame's, or NULL */
    uint8_t *exact;               /* mbw * mbh: reconstruction == source */
    uint8_t *above_ctx;           /* mbw * 9: token contexts, section 13.3 */
    uint8_t *p1;                  /* first partition is assembled here */
    size_t p1cap;
    int dq_y1[2], dq_y2[2], dq_uv[2];
    qstep qs_y1[2], qs_y2[2], qs_uv[2];
    vp8e_stats stats;
    uint32_t prof[5];

    /* The frame in progress, between vp8e_begin and vp8e_end. */
    int f_key, f_row;
    const uint8_t *f_dirty;
    vp8e_src f_src;
    uint8_t *f_out;
    size_t f_cap, f_hdr;
    bw tb;
    uint8_t left[9];
    int nskip, nintra, nchanged, nmoved;

    /* Scratch for one macroblock. Here rather than on the stack, because on
     * the Mac the encoder runs at deferred-task time on whatever stack the
     * interrupted code had. */
    uint8_t sy[256], su[64], sv[64], py[256], pu[64], pv[64], cand[256], cu[64], cv[64];
    uint8_t my[256], mu[64], mv[64];  /* motion-compensated prediction */
    mbcoef qc;

    icache_entry ic[IC_ENTRIES];
    int ic_next;
    uint8_t ickey[IC_KEY];
};

/* ---- setup ---------------------------------------------------------------- */

static size_t align4(size_t n)
{
    return (n + 3) & ~(size_t)3;
}

size_t vp8e_mem_size(int w, int h)
{
    size_t mbw = (size_t)(w + 15) / 16, mbh = (size_t)(h + 15) / 16;
    size_t mbs = mbw * mbh;
    return align4(sizeof(struct vp8e)) + align4(mbw * 16 * mbh * 16) +
           2 * align4(mbw * 8 * mbh * 8) + 2 * align4(mbs) + align4(mbw * 9) +
           align4(256 + mbs * 12) + 4 * mbs;
}

void vp8e_set_q(vp8e *e, int q)
{
    int uvdc;
    if (q < 0)
        q = 0;
    if (q > 127)
        q = 127;
    e->q = q;
    /* Section 14.1 / 20.4; every delta is zero. */
    e->dq_y1[0] = vp8_dc_qlookup[q];
    e->dq_y1[1] = vp8_ac_qlookup[q];
    e->dq_y2[0] = vp8_dc_qlookup[q] * 2;
    e->dq_y2[1] = vp8_ac_qlookup[q] * 155 / 100;
    if (e->dq_y2[1] < 8)
        e->dq_y2[1] = 8;
    uvdc = vp8_dc_qlookup[q];
    e->dq_uv[0] = uvdc > 132 ? 132 : uvdc;
    e->dq_uv[1] = vp8_ac_qlookup[q];
    {
        int i;
        for (i = 0; i < 2; i++) {
            qstep_set(&e->qs_y1[i], e->dq_y1[i]);
            qstep_set(&e->qs_y2[i], e->dq_y2[i]);
            qstep_set(&e->qs_uv[i], e->dq_uv[i]);
        }
    }
}

vp8e *vp8e_init(void *mem, int w, int h, int q)
{
    vp8e *e = (vp8e *)mem;
    uint8_t *p;
    size_t mbs;
    if (w < 16 || h < 16 || w > 16383 || h > 16383)
        return NULL;
    memset(e, 0, sizeof *e);
    norm_init();
    e->w = w;
    e->h = h;
    e->mbw = (w + 15) / 16;
    e->mbh = (h + 15) / 16;
    mbs = (size_t)e->mbw * e->mbh;
    p = (uint8_t *)mem + align4(sizeof *e);
    e->rys = e->mbw * 16;
    e->ruvs = e->mbw * 8;
    e->ry = p;
    p += align4((size_t)e->rys * e->mbh * 16);
    e->ru = p;
    p += align4((size_t)e->ruvs * e->mbh * 8);
    e->rv = p;
    p += align4((size_t)e->ruvs * e->mbh * 8);
    e->mbinfo = p;
    p += align4(mbs);
    e->exact = p;
    p += align4(mbs);
    e->above_ctx = p;
    p += align4((size_t)e->mbw * 9);
    e->p1 = p;
    e->p1cap = 256 + mbs * 12;
    p += align4(e->p1cap);
    e->mvs = (int32_t *)p;
    memset(e->mvs, 0, 4 * mbs);
    vp8e_set_q(e, q);
    return e;
}

int vp8e_mb_cols(const vp8e *e) { return e->mbw; }
const uint8_t *vp8e_exact_map(const vp8e *e) { return e->exact; }

void vp8e_profile(vp8e *e, uint32_t out[5])
{
    int i;
    for (i = 0; i < 5; i++) {
        out[i] = e->prof[i];
        e->prof[i] = 0;
    }
}
int vp8e_mb_rows(const vp8e *e) { return e->mbh; }

void vp8e_recon(const vp8e *e, const uint8_t **y, const uint8_t **u,
                const uint8_t **v, int *ys, int *uvs)
{
    *y = e->ry;
    *u = e->ru;
    *v = e->rv;
    *ys = e->rys;
    *uvs = e->ruvs;
}

void vp8e_last_stats(const vp8e *e, vp8e_stats *s)
{
    *s = e->stats;
}

size_t vp8e_ref_size(int w, int h)
{
    size_t mbw = (size_t)(w + 15) / 16, mbh = (size_t)(h + 15) / 16;
    return mbw * 16 * mbh * 16 + 2 * mbw * 8 * mbh * 8;
}

void vp8e_set_ref(vp8e *e, void *mem)
{
    e->refy = (uint8_t *)mem;
    e->refu = e->refy + (size_t)e->rys * e->mbh * 16;
    e->refv = e->refu + (size_t)e->ruvs * e->mbh * 8;
}

void vp8e_set_hints(vp8e *e, const vp8e_hint *hints)
{
    e->hints = hints;
}

/* ---- motion vectors, sections 5, 16.3, 17 and 18 ---------------------------- *
 *
 * A vector is kept as libvpx and the RFC's decoder keep it: row and column in
 * eighths of a pixel (the bitstream's quarter pixels, doubled), packed into one
 * word so equality is one compare. */

static int32_t mv_pack(int row, int col)
{
    return (int32_t)((uint32_t)(uint16_t)row << 16 | (uint16_t)col);
}
static int mv_row(int32_t m) { return (int16_t)((uint32_t)m >> 16); }
static int mv_col(int32_t m) { return (int16_t)((uint32_t)m & 0xffff); }

/* Clamp to one macroblock past the frame, as the decoder does to the three
 * vectors find_near_mvs offers (RFC 6386, vp8_clamp_mv and dixie's bounds). */
static int32_t mv_clamp(const vp8e *e, int r, int c, int32_t m)
{
    int row = mv_row(m), col = mv_col(m);
    int to_left = -((c + 1) << 7), to_right = (e->mbw - c) << 7;
    int to_top = -((r + 1) << 7), to_bottom = (e->mbh - r) << 7;
    if (col < to_left)
        col = to_left;
    if (col > to_right)
        col = to_right;
    if (row < to_top)
        row = to_top;
    if (row > to_bottom)
        row = to_bottom;
    return mv_pack(row, col);
}

typedef struct {
    int32_t best, nearest, near;
    int cnt[4];
} nearmv;

/* vp8_find_near_mvs, for a stream with no SPLITMV and one reference frame
 * (so no sign bias): the census of the above, left and above-left vectors.
 * Off-frame neighbours count as intra, as the decoder's border does. */
static void find_near(const vp8e *e, int r, int c, nearmv *n)
{
    const uint8_t *info = e->mbinfo;
    int32_t near_mvs[4] = { 0, 0, 0, 0 };
    int k = 0, mbw = e->mbw, *cnt = n->cnt;
    cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
    if (!e->nmoved) {
        /* No vector anywhere in this frame (most frames): the census is a
         * weighted count of inter neighbours, and every vector is zero --
         * zero needs no clamping. */
        if (r > 0 && (info[(r - 1) * mbw + c] & MB_INTER))
            cnt[0] += 2;
        if (c > 0 && (info[r * mbw + c - 1] & MB_INTER))
            cnt[0] += 2;
        if (r > 0 && c > 0 && (info[(r - 1) * mbw + c - 1] & MB_INTER))
            cnt[0] += 1;
        n->best = n->nearest = n->near = 0;
        return;
    }
    if (r > 0 && (info[(r - 1) * mbw + c] & MB_INTER)) {
        int32_t m = e->mvs[(r - 1) * mbw + c];
        if (m)
            near_mvs[++k] = m;
        cnt[k] += 2;
    }
    if (c > 0 && (info[r * mbw + c - 1] & MB_INTER)) {
        int32_t m = e->mvs[r * mbw + c - 1];
        if (m) {
            if (m != near_mvs[k])
                near_mvs[++k] = m;
            cnt[k] += 2;
        } else {
            cnt[0] += 2;
        }
    }
    if (r > 0 && c > 0 && (info[(r - 1) * mbw + c - 1] & MB_INTER)) {
        int32_t m = e->mvs[(r - 1) * mbw + c - 1];
        if (m) {
            if (m != near_mvs[k])
                near_mvs[++k] = m;
            cnt[k] += 1;
        } else {
            cnt[0] += 1;
        }
    }
    if (cnt[3] && near_mvs[k] == near_mvs[1]) /* three distinct: merge above-left */
        cnt[1] += 1;
    cnt[3] = 0; /* no neighbour is ever SPLITMV */
    if (cnt[2] > cnt[1]) {
        int t = cnt[1];
        int32_t m = near_mvs[1];
        cnt[1] = cnt[2];
        cnt[2] = t;
        near_mvs[1] = near_mvs[2];
        near_mvs[2] = m;
    }
    if (cnt[1] >= cnt[0])
        near_mvs[0] = near_mvs[1];
    n->best = mv_clamp(e, r, c, near_mvs[0]);
    n->nearest = mv_clamp(e, r, c, near_mvs[1]);
    n->near = mv_clamp(e, r, c, near_mvs[2]);
}

enum { MV_ZERO, MV_NEAREST, MV_NEAR, MV_NEW };

/* A moved block is sent as its vector alone when no pixel is further than
 * this from the prediction, and the mean difference is at most MOVE_MEAN:
 * the coding error the block carried already (a few levels; tens at a sharp
 * edge), not new strokes (a hundred and more). */
#define MOVE_TOLERANCE 48
#define MOVE_MEAN 12

/* How a vector would be sent here, or -1 if it cannot be (NEWMV codes the
 * difference from best, in quarter pixels, up to 1023 a component). */
static int mv_mode(const nearmv *n, int32_t m)
{
    int dr, dc;
    if (!m)
        return MV_ZERO;
    if (m == n->nearest)
        return MV_NEAREST;
    if (m == n->near)
        return MV_NEAR;
    dr = (mv_row(m) - mv_row(n->best)) >> 1;
    dc = (mv_col(m) - mv_col(n->best)) >> 1;
    if (dr < -1023 || dr > 1023 || dc < -1023 || dc > 1023)
        return -1;
    return MV_NEW;
}

/* One component of a NEWMV, section 17 (the encoder's half of
 * read_mvcomponent). */
static void put_mv_component(bw *b, int v, const uint8_t *p)
{
    enum { IS_SHORT = 0, SIGN = 1, SHORT = 2, BITS = 9 };
    static const signed char small_tree[14] = { 2, 8, 4, 6, -0, -1, -2, -3, 10, 12, -4, -5, -6, -7 };
    int x = v < 0 ? -v : v, i;
    if (x < 8) {
        bw_put(b, p[IS_SHORT], 0);
        bw_tree(b, small_tree, p + SHORT, x);
        if (!x)
            return;
    } else {
        bw_put(b, p[IS_SHORT], 1);
        for (i = 0; i < 3; i++)
            bw_put(b, p[BITS + i], (x >> i) & 1);
        for (i = 9; i > 3; i--)
            bw_put(b, p[BITS + i], (x >> i) & 1);
        if (x & 0xfff0)
            bw_put(b, p[BITS + 3], (x >> 3) & 1);
    }
    bw_put(b, p[SIGN], v < 0);
}

/* The six-tap predictor of section 18 for an 8x8 chroma block (libvpx's
 * vp8_sixtap_predict8x8_c): horizontal then vertical, each pass rounded and
 * clamped. xf, yf are eighths of a pixel. */
/* One tap sum. The whole-pixel filter is (128 * p + 64) >> 7 = p, so a pass
 * at offset 0 changes nothing and is skipped; the half-pixel one, the only
 * other a whole-pixel luma vector gives chroma, is spelt out in constants,
 * which a compiler turns into shifts and adds -- a multiply is 12 cycles on
 * an R3000. */
#define TAPS(a, b, c, d, e2, f, k)                                                             \
    ((k) == 4 ? (3 * ((a) + (f)) - 16 * ((b) + (e2)) + 77 * ((c) + (d)) + 64) >> 7              \
              : ((a) * fl[0] + (b) * fl[1] + (c) * fl[2] + (d) * fl[3] + (e2) * fl[4] +        \
                 (f) * fl[5] + 64) >> 7)

static uint8_t clamp_tap(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void sixtap8x8(const uint8_t *src, int stride, int xf, int yf, uint8_t *dst)
{
    int tmp[13 * 8], i, j;
    if (!xf || !yf) {
        /* One direction only: the other pass is the identity. */
        const short *fl = vp8_sixtap_filters[xf | yf];
        int k = xf | yf, step = xf ? 1 : stride;
        for (j = 0; j < 8; j++, src += stride)
            for (i = 0; i < 8; i++) {
                const uint8_t *p = src + i;
                dst[j * 8 + i] = clamp_tap(TAPS(p[-2 * step], p[-step], p[0], p[step],
                                                p[2 * step], p[3 * step], k));
            }
        return;
    }
    const uint8_t *s = src - 2 * stride;
    const short *fl = vp8_sixtap_filters[xf];
    for (j = 0; j < 13; j++, s += stride)
        for (i = 0; i < 8; i++)
            tmp[j * 8 + i] = xf ? clamp_tap(TAPS(s[i - 2], s[i - 1], s[i], s[i + 1], s[i + 2],
                                                 s[i + 3], xf))
                                : s[i];
    fl = vp8_sixtap_filters[yf];
    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++) {
            const int *t = tmp + (j + 2) * 8 + i;
            dst[j * 8 + i] = yf ? clamp_tap(TAPS(t[-16], t[-8], t[0], t[8], t[16], t[24], yf))
                                : (uint8_t)t[0];
        }
}

/* The prediction a whole-macroblock vector gives at (r, c), from the
 * previous frame, into e->my/mu/mv. 0 if it reads outside the frame (the
 * decoder would extend the edges; this encoder does not offer such vectors). */
static int motion_predict(vp8e *e, int r, int c, int32_t m)
{
    int row = mv_row(m), col = mv_col(m), j;
    int x = c * 16 + (col >> 3), y = r * 16 + (row >> 3);
    int urow = (row + 1 + (row >> 31) * 2) / 2, ucol = (col + 1 + (col >> 31) * 2) / 2;
    int ux = c * 8 + (ucol >> 3), uy = r * 8 + (urow >> 3), uxf = ucol & 7, uyf = urow & 7;
    int fw = e->mbw * 16, fh = e->mbh * 16;
    if ((row | col) & 7)
        return 0; /* whole pixels only */
    if (x < 0 || y < 0 || x + 16 > fw || y + 16 > fh)
        return 0;
    if (ux - 2 < 0 || uy - 2 < 0 || ux + 8 + 3 > fw / 2 || uy + 8 + 3 > fh / 2)
        return 0;
    for (j = 0; j < 16; j++)
        copy_row(e->my + j * 16, e->refy + (y + j) * e->rys + x, 16);
    if (uxf | uyf) {
        sixtap8x8(e->refu + uy * e->ruvs + ux, e->ruvs, uxf, uyf, e->mu);
        sixtap8x8(e->refv + uy * e->ruvs + ux, e->ruvs, uxf, uyf, e->mv);
    } else {
        for (j = 0; j < 8; j++) {
            copy_row(e->mu + j * 8, e->refu + (uy + j) * e->ruvs + ux, 8);
            copy_row(e->mv + j * 8, e->refv + (uy + j) * e->ruvs + ux, 8);
        }
    }
    return 1;
}

static int32_t hint_mv(const vp8e_hint *h)
{
    return mv_pack(-8 * h->dy, -8 * h->dx);
}

/* Copy, before the frame changes it, the part of the picture this frame's
 * hints read from. */
static void snapshot_ref(vp8e *e)
{
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, r, c, j;
    if (!e->refy || !e->hints)
        return;
    for (r = 0; r < e->mbh; r++)
        for (c = 0; c < e->mbw; c++) {
            const vp8e_hint *h = &e->hints[r * e->mbw + c];
            int x, y;
            if (!h->dx && !h->dy)
                continue;
            if (e->f_dirty && !e->f_dirty[r * e->mbw + c])
                continue;
            x = c * 16 - h->dx;
            y = r * 16 - h->dy;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x + 16 > x1) x1 = x + 16;
            if (y + 16 > y1) y1 = y + 16;
        }
    if (x1 < 0)
        return;
    /* the chroma filter's reach, in luma pixels, and the frame */
    x0 -= 6; y0 -= 6; x1 += 8; y1 += 8;
    x0 = x0 < 0 ? 0 : x0 & ~1;
    y0 = y0 < 0 ? 0 : y0 & ~1;
    if (x1 > e->mbw * 16) x1 = e->mbw * 16;
    if (y1 > e->mbh * 16) y1 = e->mbh * 16;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (j = y0; j < y1; j++)
        memcpy(e->refy + j * e->rys + x0, e->ry + j * e->rys + x0, (size_t)(x1 - x0));
    for (j = y0 / 2; j < (y1 + 1) / 2; j++) {
        memcpy(e->refu + j * e->ruvs + x0 / 2, e->ru + j * e->ruvs + x0 / 2, (size_t)(x1 - x0) / 2);
        memcpy(e->refv + j * e->ruvs + x0 / 2, e->rv + j * e->ruvs + x0 / 2, (size_t)(x1 - x0) / 2);
    }
}

/* ---- prediction ----------------------------------------------------------- */

/* Build an n x n intra predictor (n = 16 luma, 8 chroma) from the
 * reconstruction around (x, y), section 12.2. Only modes whose edges exist are
 * ever asked for, except DC, which copes with missing edges itself. */
static void intra_pred(const uint8_t *rec, int stride, int x, int y, int n, int mode,
                       uint8_t *pred)
{
    const uint8_t *above = rec + (y - 1) * stride + x;
    const uint8_t *left = rec + y * stride + x - 1;
    int i, j;
    if (mode == DC_PRED) {
        int sum = 0, cnt = 0, shift = n == 16 ? 3 : 2, dc;
        if (y > 0) {
            for (i = 0; i < n; i++)
                sum += above[i];
            shift++;
            cnt++;
        }
        if (x > 0) {
            for (i = 0; i < n; i++)
                sum += left[i * stride];
            shift++;
            cnt++;
        }
        dc = cnt ? (sum + (1 << (shift - 1))) >> shift : 128;
        memset(pred, dc, (size_t)n * n);
    } else if (mode == V_PRED) {
        for (j = 0; j < n; j++)
            memcpy(pred + j * n, above, (size_t)n);
    } else if (mode == H_PRED) {
        for (j = 0; j < n; j++)
            memset(pred + j * n, left[j * stride], (size_t)n);
    } else { /* TM_PRED */
        int p = above[-1];
        for (j = 0; j < n; j++) {
            int l = left[j * stride] - p;
            for (i = 0; i < n; i++)
                pred[j * n + i] = clamp255(l + above[i]);
        }
    }
}

/* Sum of absolute differences, giving up (with some sum >= limit) once it
 * cannot beat `limit`: the caller only keeps a candidate that is strictly
 * better, so the decision is the same as with the whole sum. Checked every
 * 16 pixels -- one row of a macroblock. */
/* The largest single difference, and the sum, of two blocks. */
static int maxdiff(const uint8_t *a, const uint8_t *b, int n, int *sum)
{
    int m = 0, s = 0, i;
    for (i = 0; i < n; i++) {
        int d = a[i] - b[i];
        d = d < 0 ? -d : d;
        s += d;
        if (d > m)
            m = d;
    }
    *sum = s;
    return m;
}

static int sad(const uint8_t *a, const uint8_t *b, int n, int limit)
{
    int s = 0;
    const uint8_t *end = a + n;
#define SAD1(k) do { int d = a[k] - b[k]; s += d < 0 ? -d : d; } while (0)
    while (a < end) {
        SAD1(0); SAD1(1); SAD1(2); SAD1(3); SAD1(4); SAD1(5); SAD1(6); SAD1(7);
        a += 8;
        b += 8;
        if (s >= limit)
            break;
    }
#undef SAD1
    return s;
}

/* Copy an n x n block out of the source, repeating the last row and column
 * where the macroblock hangs over the picture's edge. */
static void load_src(const uint8_t *plane, int stride, int pw, int ph, int x, int y, int n,
                     uint8_t *dst)
{
    int i, j;
    for (j = 0; j < n; j++) {
        int sy = y + j < ph ? y + j : ph - 1;
        const uint8_t *row = plane + sy * stride;
        if (x + n <= pw) {
            copy_row(dst + j * n, row + x, n);
        } else {
            for (i = 0; i < n; i++)
                dst[j * n + i] = row[x + i < pw ? x + i : pw - 1];
        }
    }
}

static void load_rec(const uint8_t *rec, int stride, int x, int y, int n, uint8_t *dst)
{
    int j;
    for (j = 0; j < n; j++)
        copy_row(dst + j * n, rec + (y + j) * stride + x, n);
}

/* ---- one macroblock ------------------------------------------------------- */




/* Residual of one 4x4 block at (bx, by) inside an n-wide block. */
/* Returns nonzero if any of it is. A block the prediction got exactly --
 * most of a screen's, flat background around a few strokes -- needs no
 * transform: its coefficients are all zero. */
static int residual4(const uint8_t *src, const uint8_t *pred, int n, int bx, int by, short *d)
{
    int i, j, any = 0;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            int o = (by + j) * n + bx + i;
            d[j * 4 + i] = (short)(src[o] - pred[o]);
            any |= d[j * 4 + i];
        }
    return any;
}

/* Transform and quantise a macroblock; `qc` is filled in zigzag order.
 * Returns nonzero if any coefficient survived. */
static int quantise_mb(const vp8e *e, const uint8_t *sy, const uint8_t *py, const uint8_t *su,
                       const uint8_t *pu, const uint8_t *sv, const uint8_t *pv, mbcoef *qc)
{
    short d[16], c[16], dcs[16], w[16];
    int b, k, any = 0;
    for (b = 0; b < 16; b++) {
        if (!residual4(sy, py, 16, (b & 3) * 4, (b >> 2) * 4, d)) {
            dcs[b] = 0;
            memset(qc->y[b], 0, sizeof qc->y[b]);
            continue;
        }
        fdct4x4(d, c);
        dcs[b] = c[0];
        qc->y[b][0] = 0;
        for (k = 1; k < 16; k++) {
            qc->y[b][k] = quant(c[zigzag[k]], &e->qs_y1[1]);
            any |= qc->y[b][k];
        }
    }
    fwalsh(dcs, w);
    for (k = 0; k < 16; k++) {
        qc->y2[k] = quant(w[zigzag[k]], &e->qs_y2[k ? 1 : 0]);
        any |= qc->y2[k];
    }
    for (b = 0; b < 8; b++) {
        const uint8_t *s = b < 4 ? su : sv;
        const uint8_t *p = b < 4 ? pu : pv;
        if (!residual4(s, p, 8, (b & 1) * 4, ((b & 3) >> 1) * 4, d)) {
            memset(qc->uv[b], 0, sizeof qc->uv[b]);
            continue;
        }
        fdct4x4(d, c);
        for (k = 0; k < 16; k++) {
            qc->uv[b][k] = quant(c[zigzag[k]], &e->qs_uv[k ? 1 : 0]);
            any |= qc->uv[b][k];
        }
    }
    return any != 0;
}

/* Dequantise, inverse transform and write the macroblock into the
 * reconstruction -- the same arithmetic the decoder does. */
static void reconstruct_mb(vp8e *e, int mx, int my, const uint8_t *py, const uint8_t *pu,
                           const uint8_t *pv, const mbcoef *qc, int coded)
{
    uint8_t *ry = e->ry + my * 16 * e->rys + mx * 16;
    uint8_t *ru = e->ru + my * 8 * e->ruvs + mx * 8;
    uint8_t *rv = e->rv + my * 8 * e->ruvs + mx * 8;
    short c[16], y2[16], dcs[16];
    int b, k, j;
    if (!coded) {
        for (j = 0; j < 16; j++)
            copy_row(ry + j * e->rys, py + j * 16, 16);
        for (j = 0; j < 8; j++) {
            copy_row(ru + j * e->ruvs, pu + j * 8, 8);
            copy_row(rv + j * e->ruvs, pv + j * 8, 8);
        }
        return;
    }
    memset(y2, 0, sizeof y2);
    for (k = 0; k < 16; k++)
        y2[zigzag[k]] = (short)(qc->y2[k] * e->dq_y2[k ? 1 : 0]);
    iwalsh(y2, dcs);
    for (b = 0; b < 16; b++) {
        int bx = (b & 3) * 4, by = (b >> 2) * 4, ac = 0;
        for (k = 1; k < 16; k++)
            ac |= qc->y[b][k];
        if (!ac) {
            idct_dc_add(dcs[b], py + by * 16 + bx, 16, ry + by * e->rys + bx, e->rys);
            continue;
        }
        memset(c, 0, sizeof c);
        c[0] = dcs[b];
        for (k = 1; k < 16; k++)
            c[zigzag[k]] = (short)(qc->y[b][k] * e->dq_y1[1]);
        idct_add(c, py + by * 16 + bx, 16, ry + by * e->rys + bx, e->rys);
    }
    for (b = 0; b < 8; b++) {
        const uint8_t *p = b < 4 ? pu : pv;
        uint8_t *r = b < 4 ? ru : rv;
        int bx = (b & 1) * 4, by = ((b & 3) >> 1) * 4, ac = 0;
        for (k = 1; k < 16; k++)
            ac |= qc->uv[b][k];
        if (!ac) {
            idct_dc_add((short)(qc->uv[b][0] * e->dq_uv[0]), p + by * 8 + bx, 8, r + by * e->ruvs + bx,
                        e->ruvs);
            continue;
        }
        memset(c, 0, sizeof c);
        for (k = 0; k < 16; k++)
            c[zigzag[k]] = (short)(qc->uv[b][k] * e->dq_uv[k ? 1 : 0]);
        idct_add(c, p + by * 8 + bx, 8, r + by * e->ruvs + bx, e->ruvs);
    }
}

/* Token contexts: 4 luma columns/rows, 2 U, 2 V, 1 Y2 (section 13.3). */
static void put_mb_tokens(bw *b, const mbcoef *qc, uint8_t *above, uint8_t *left)
{
    int i;
    above[8] = left[8] = (uint8_t)put_block(b, qc->y2, 1, 0, above[8] + left[8]);
    for (i = 0; i < 16; i++) {
        uint8_t *a = &above[i & 3], *l = &left[i >> 2];
        *a = *l = (uint8_t)put_block(b, qc->y[i], 0, 1, *a + *l);
    }
    for (i = 0; i < 8; i++) {
        int base = i < 4 ? 4 : 6, j = i & 3;
        uint8_t *a = &above[base + (j & 1)], *l = &left[base + (j >> 1)];
        *a = *l = (uint8_t)put_block(b, qc->uv[i], 2, 0, *a + *l);
    }
}

/* ---- frame ---------------------------------------------------------------- */

static int prob_of(int zeros, int total)
{
    int p;
    if (!total)
        return 128;
    p = (zeros * 256 + total / 2) / total;
    return p < 1 ? 1 : p > 255 ? 255 : p;
}

static void put_header(vp8e *e, bw *b, int key, int p_skip, int p_intra)
{
    int i, j, k, l;
    if (key) {
        bw_literal(b, 0, 1); /* color_space */
        bw_literal(b, 0, 1); /* clamping_type */
    }
    bw_literal(b, 0, 1); /* segmentation_enabled */
    bw_literal(b, 0, 1); /* filter_type */
    bw_literal(b, 0, 6); /* loop_filter_level: off, so the reconstruction is exact */
    bw_literal(b, 0, 3); /* sharpness_level */
    bw_literal(b, 0, 1); /* loop_filter_adj_enable */
    bw_literal(b, 0, 2); /* log2_nbr_of_dct_partitions */
    bw_literal(b, (uint32_t)e->q, 7);
    for (i = 0; i < 5; i++)
        bw_literal(b, 0, 1); /* no quantiser deltas */
    if (key) {
        bw_literal(b, 1, 1); /* refresh_entropy_probs */
    } else {
        bw_literal(b, 0, 1); /* refresh_golden_frame */
        bw_literal(b, 0, 1); /* refresh_alternate_frame */
        bw_literal(b, 0, 2); /* copy_buffer_to_golden */
        bw_literal(b, 0, 2); /* copy_buffer_to_alternate */
        bw_literal(b, 0, 1); /* sign_bias_golden */
        bw_literal(b, 0, 1); /* sign_bias_alternate */
        bw_literal(b, 1, 1); /* refresh_entropy_probs */
        bw_literal(b, 1, 1); /* refresh_last */
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            for (k = 0; k < 3; k++)
                for (l = 0; l < 11; l++)
                    bw_put(b, vp8_coef_update_probs[i][j][k][l], 0);
    bw_literal(b, 1, 1); /* mb_no_coeff_skip */
    bw_literal(b, (uint32_t)p_skip, 8);
    if (!key) {
        bw_literal(b, (uint32_t)p_intra, 8);
        bw_literal(b, 255, 8); /* prob_last: every inter macroblock uses LAST */
        bw_literal(b, 128, 8); /* prob_gf: unused */
        bw_literal(b, 0, 1);   /* intra_16x16_prob_update_flag */
        bw_literal(b, 0, 1);   /* intra_chroma_prob_update_flag */
        for (i = 0; i < 2; i++)
            for (j = 0; j < 19; j++)
                bw_put(b, vp8_mv_update_probs[i][j], 0);
    }
}

static void put_modes(vp8e *e, bw *b, int key, int p_skip, int p_intra)
{
    int r, c;
    for (r = 0; r < e->mbh; r++)
        for (c = 0; c < e->mbw; c++) {
            const uint8_t *mi = e->mbinfo + r * e->mbw + c;
            int info = *mi;
            bw_put(b, p_skip, (info & MB_SKIP) != 0);
            if (key) {
                bw_tree(b, kf_ymode_tree, kf_ymode_prob, MB_YMODE(info));
                bw_tree(b, uv_mode_tree, kf_uv_mode_prob, MB_UVMODE(info));
                continue;
            }
            bw_put(b, p_intra, info & MB_INTER);
            if (info & MB_INTER) {
                /* Section 16.3: the mode is coded against the census of the
                 * neighbours' vectors. */
                nearmv nm;
                int32_t m = e->mvs[r * e->mbw + c];
                int mode;
                find_near(e, r, c, &nm);
                mode = mv_mode(&nm, m);
                bw_put(b, 255, 0); /* mb_ref_frame_sel1 = LAST (prob_last) */
                bw_put(b, vp8_mode_contexts[nm.cnt[0]][0], mode != MV_ZERO);
                if (mode != MV_ZERO) {
                    bw_put(b, vp8_mode_contexts[nm.cnt[1]][1], mode != MV_NEAREST);
                    if (mode != MV_NEAREST) {
                        bw_put(b, vp8_mode_contexts[nm.cnt[2]][2], mode != MV_NEAR);
                        if (mode == MV_NEW) {
                            bw_put(b, vp8_mode_contexts[nm.cnt[3]][3], 0); /* not split */
                            put_mv_component(b, (mv_row(m) - mv_row(nm.best)) >> 1,
                                             vp8_default_mv_context[0]);
                            put_mv_component(b, (mv_col(m) - mv_col(nm.best)) >> 1,
                                             vp8_default_mv_context[1]);
                        }
                    }
                }
            } else {
                bw_tree(b, ymode_tree, ymode_prob, MB_YMODE(info));
                bw_tree(b, uv_mode_tree, uv_mode_prob, MB_UVMODE(info));
            }
        }
}

/* The intra cache key: source, availability, and every reconstructed pixel an
 * intra predictor of this macroblock could read. */
static void ic_make_key(vp8e *e, int r, int c)
{
    uint8_t *k = e->ickey;
    int j;
    memcpy(k, e->sy, 256);
    memcpy(k + 256, e->su, 64);
    memcpy(k + 320, e->sv, 64);
    k += 384;
    memset(k, 0, IC_KEY - 384);
    *k++ = (uint8_t)((r > 0) | (c > 0) << 1);
    if (r > 0) {
        const uint8_t *a = e->ry + (r * 16 - 1) * e->rys + c * 16;
        k[0] = c > 0 ? a[-1] : 0;
        memcpy(k + 1, a, 16);
    }
    k += 17;
    if (c > 0)
        for (j = 0; j < 16; j++)
            k[j] = e->ry[(r * 16 + j) * e->rys + c * 16 - 1];
    k += 16;
    {
        const uint8_t *planes[2];
        int pl;
        planes[0] = e->ru;
        planes[1] = e->rv;
        for (pl = 0; pl < 2; pl++) {
            if (r > 0) {
                const uint8_t *a = planes[pl] + (r * 8 - 1) * e->ruvs + c * 8;
                k[0] = c > 0 ? a[-1] : 0;
                memcpy(k + 1, a, 8);
            }
            k += 9;
            if (c > 0)
                for (j = 0; j < 8; j++)
                    k[j] = planes[pl][(r * 8 + j) * e->ruvs + c * 8 - 1];
            k += 8;
        }
    }
}

static void recon_block(vp8e *e, int r, int c, uint8_t *y, uint8_t *u, uint8_t *v, int store)
{
    int j;
    for (j = 0; j < 16; j++) {
        uint8_t *row = e->ry + (r * 16 + j) * e->rys + c * 16;
        if (store)
            memcpy(y + j * 16, row, 16);
        else
            memcpy(row, y + j * 16, 16);
    }
    for (j = 0; j < 8; j++) {
        uint8_t *ru = e->ru + (r * 8 + j) * e->ruvs + c * 8;
        uint8_t *rv = e->rv + (r * 8 + j) * e->ruvs + c * 8;
        if (store) {
            memcpy(u + j * 8, ru, 8);
            memcpy(v + j * 8, rv, 8);
        } else {
            memcpy(ru, u + j * 8, 8);
            memcpy(rv, v + j * 8, 8);
        }
    }
}

int vp8e_begin(vp8e *e, const vp8e_src *src, const uint8_t *dirty, int key, uint8_t *out,
               size_t cap)
{
    if (!e->have_ref)
        key = 1;
    e->f_key = key;
    e->f_hdr = key ? 10 : 3;
    if (cap < e->f_hdr + e->p1cap + 64)
        return 0;
    e->f_src = *src;
    e->f_dirty = dirty;
    e->f_out = out;
    e->f_cap = cap;
    e->f_row = 0;
    e->nskip = e->nintra = e->nchanged = e->nmoved = 0;
    if (key)
        e->hints = NULL;
    snapshot_ref(e);
    {
        int i;
        for (i = 0; i < IC_ENTRIES; i++)
            e->ic[i].valid = 0;
    }
    bw_init(&e->tb, out + e->f_hdr + e->p1cap, out + cap);
    memset(e->above_ctx, 0, (size_t)e->mbw * 9);
    return 1;
}

int vp8e_rows(vp8e *e, int nrows)
{
    static const int ymodes[4] = { DC_PRED, V_PRED, H_PRED, TM_PRED };
    const vp8e_src *src = &e->f_src;
    const int cw = (e->w + 1) / 2, ch = (e->h + 1) / 2, key = e->f_key;
    uint8_t *left = e->left, *sy = e->sy, *su = e->su, *sv = e->sv;
    uint8_t *py = e->py, *pu = e->pu, *pv = e->pv, *cand = e->cand;
    mbcoef *qc = &e->qc;
    int c;

    for (; nrows > 0 && e->f_row < e->mbh; nrows--, e->f_row++) {
        int r = e->f_row;
        memset(left, 0, 9);
        for (c = 0; c < e->mbw; c++) {
            uint8_t *above = e->above_ctx + c * 9;
            uint8_t *info = e->mbinfo + r * e->mbw + c;
            int best, bestuv, bestmode, inter = 0, m, coded, uvmode = DC_PRED;
            int32_t moved;
            int moved_only = 0, moved_exact = 0;
            PROF_START;

            e->mvs[r * e->mbw + c] = 0;
            if (!key && e->f_dirty && !e->f_dirty[r * e->mbw + c]) {
                /* Unchanged: copy from the last frame, nothing coded. */
                int z;
                *info = MB_INTER | MB_SKIP;
                for (z = 0; z < 9; z++)
                    above[z] = left[z] = 0;
                e->nskip++;
                continue;
            }

            load_src(src->y, src->ystride, e->w, e->h, c * 16, r * 16, 16, sy);
            load_src(src->u, src->uvstride, cw, ch, c * 8, r * 8, 8, su);
            load_src(src->v, src->uvstride, cw, ch, c * 8, r * 8, 8, sv);

            if (key) {
                icache_entry *hit = NULL;
                int i;
                ic_make_key(e, r, c);
                for (i = 0; i < IC_ENTRIES; i++)
                    if (e->ic[i].valid && !memcmp(e->ic[i].key, e->ickey, IC_KEY)) {
                        hit = &e->ic[i];
                        break;
                    }
                if (hit) {
                    recon_block(e, r, c, hit->ry, hit->ru, hit->rv, 0);
                    e->exact[r * e->mbw + c] = hit->exact;
                    *info = (uint8_t)((hit->coded ? 0 : MB_SKIP) | (hit->mode << 1) |
                                      (hit->uvmode << 4));
                    e->nchanged++;
                    e->nintra++;
                    if (hit->coded) {
                        put_mb_tokens(&e->tb, &hit->qc, above, left);
                    } else {
                        memset(above, 0, 9);
                        memset(left, 0, 9);
                        e->nskip++;
                    }
                    PROF(4);
                    continue;
                }
            }

            /* Luma: the closest of the last frame and the intra modes whose
             * edges exist. Ties go to the last frame, which is cheaper. */
            best = 0x7fffffff;
            bestmode = DC_PRED;
            moved = 0;
            if (!key) {
                int s2 = 0x7fffffff, md = 256, mdu = 256, mdv = 256;
                inter = 1;
                /* Where the platform says these contents came from, first:
                 * moved and otherwise the same, give or take the coding
                 * error the block already carried, it is sent as the vector
                 * alone -- no residual, no other candidate looked at. The
                 * peer sees what it saw before, moved; the exactness map
                 * says so, for a later refresh. New content differs by far
                 * more than coding error does. */
                if (e->hints && e->refy) {
                    const vp8e_hint *h = &e->hints[r * e->mbw + c];
                    int32_t m;
                    nearmv nm;
                    if ((h->dx || h->dy) && motion_predict(e, r, c, m = hint_mv(h))) {
                        find_near(e, r, c, &nm);
                        if (mv_mode(&nm, m) >= 0) {
                            int su2, sv2;
                            md = maxdiff(sy, e->my, 256, &s2);
                            moved = m;
                            if (md <= MOVE_TOLERANCE && s2 <= 256 * MOVE_MEAN &&
                                (mdu = maxdiff(su, e->mu, 64, &su2)) <= MOVE_TOLERANCE &&
                                (mdv = maxdiff(sv, e->mv, 64, &sv2)) <= MOVE_TOLERANCE)
                                moved_only = 1;
                        }
                    }
                }
                if (moved_only) {
                    best = s2;
                    memcpy(py, e->my, 256);
                    moved_exact = !md && !mdu && !mdv;
                } else {
                    load_rec(e->ry, e->rys, c * 16, r * 16, 16, py);
                    best = sad(sy, py, 256, 0x7fffffff);
                    if (moved && s2 < best) {
                        best = s2;
                        memcpy(py, e->my, 256);
                    } else {
                        moved = 0;
                    }
                }
            }
            for (m = 0; m < 4 && best && !moved_only; m++) {
                int mode = ymodes[m], s;
                if ((mode == V_PRED && r == 0) || (mode == H_PRED && c == 0) ||
                    (mode == TM_PRED && (r == 0 || c == 0)))
                    continue;
                intra_pred(e->ry, e->rys, c * 16, r * 16, 16, mode, cand);
                s = sad(sy, cand, 256, best);
                if (s < best) {
                    best = s;
                    bestmode = mode;
                    inter = 0;
                    moved = 0;
                    memcpy(py, cand, 256);
                }
            }

            bestuv = 0x7fffffff;
            if (inter && moved) {
                memcpy(pu, e->mu, 64);
                memcpy(pv, e->mv, 64);
                if (moved_only)
                    best = bestuv = 0; /* nothing to code */
                else if (!best)
                    bestuv = sad(su, pu, 64, 0x7fffffff) + sad(sv, pv, 64, 0x7fffffff);
            } else if (inter) {
                load_rec(e->ru, e->ruvs, c * 8, r * 8, 8, pu);
                load_rec(e->rv, e->ruvs, c * 8, r * 8, 8, pv);
                if (!best)
                    bestuv = sad(su, pu, 64, 0x7fffffff) + sad(sv, pv, 64, 0x7fffffff);
            } else {
                for (m = 0; m < 4; m++) {
                    int mode = ymodes[m], s;
                    if ((mode == V_PRED && r == 0) || (mode == H_PRED && c == 0) ||
                        (mode == TM_PRED && (r == 0 || c == 0)))
                        continue;
                    intra_pred(e->ru, e->ruvs, c * 8, r * 8, 8, mode, e->cu);
                    intra_pred(e->rv, e->ruvs, c * 8, r * 8, 8, mode, e->cv);
                    s = sad(su, e->cu, 64, bestuv);
                    if (s < bestuv)
                        s += sad(sv, e->cv, 64, bestuv - s);
                    if (s < bestuv) {
                        bestuv = s;
                        uvmode = mode;
                        memcpy(pu, e->cu, 64);
                        memcpy(pv, e->cv, 64);
                    }
                }
            }

            PROF(0);
            /* A prediction that is already exact leaves nothing to code, and
             * the transforms would only prove it. */
            coded = best || bestuv ? quantise_mb(e, sy, py, su, pu, sv, pv, qc) : 0;
            PROF(1);
            reconstruct_mb(e, c, r, py, pu, pv, qc, coded);
            PROF(2);
            {
                /* Did the peer get the source exactly? (Edge macroblocks
                 * compare their padding too, and may never say yes; the
                 * refresh just keeps visiting them.) */
                uint8_t *ex = e->exact + r * e->mbw + c;
                int j, same = 1;
                if (moved_only) {
                    *ex = (uint8_t)moved_exact;
                    goto exact_done;
                }
                load_rec(e->ry, e->rys, c * 16, r * 16, 16, e->cand);
                same = !memcmp(e->cand, sy, 256);
                for (j = 0; j < 8 && same; j++)
                    same = !memcmp(e->ru + (r * 8 + j) * e->ruvs + c * 8, su + j * 8, 8) &&
                           !memcmp(e->rv + (r * 8 + j) * e->ruvs + c * 8, sv + j * 8, 8);
                *ex = (uint8_t)same;
            }
        exact_done:
            PROF(3);
            if (key) {
                icache_entry *slot = &e->ic[e->ic_next];
                e->ic_next = (e->ic_next + 1) % IC_ENTRIES;
                memcpy(slot->key, e->ickey, IC_KEY);
                slot->mode = (uint8_t)bestmode;
                slot->uvmode = (uint8_t)uvmode;
                slot->coded = (uint8_t)coded;
                slot->exact = e->exact[r * e->mbw + c];
                if (coded)
                    slot->qc = *qc;
                recon_block(e, r, c, slot->ry, slot->ru, slot->rv, 1);
                slot->valid = 1;
            }
            *info = (uint8_t)((inter ? MB_INTER : 0) | (coded ? 0 : MB_SKIP) |
                              (bestmode << 1) | (uvmode << 4));
            e->mvs[r * e->mbw + c] = inter ? moved : 0;
            if (moved && inter)
                e->nmoved++;
            /* Only an inter macroblock with nothing coded and no vector is
             * a straight copy of what the peer already has. */
            if (coded || !inter || moved)
                e->nchanged++;
            if (coded) {
                put_mb_tokens(&e->tb, qc, above, left);
            } else {
                memset(above, 0, 9);
                memset(left, 0, 9);
                e->nskip++;
            }
            if (!inter)
                e->nintra++;
            PROF(4);
        }
    }
    return e->f_row >= e->mbh;
}

size_t vp8e_end(vp8e *e)
{
    uint8_t *out = e->f_out;
    size_t hdr = e->f_hdr, p1len, p2len;
    int total = e->mbw * e->mbh, key = e->f_key, p_skip, p_intra;
    bw pb;

    p2len = bw_finish(&e->tb);
    p_skip = prob_of(total - e->nskip, total);
    p_intra = prob_of(e->nintra, total);
    bw_init(&pb, e->p1, e->p1 + e->p1cap);
    put_header(e, &pb, key, p_skip, p_intra);
    put_modes(e, &pb, key, p_skip, p_intra);
    p1len = bw_finish(&pb);

    if (e->tb.overflow || pb.overflow || p1len >= (1u << 19)) {
        e->have_ref = 0;
        return 0;
    }

    /* Frame tag, section 9.1: keyframe bit (inverted), version 0, shown. */
    {
        uint32_t tag = (key ? 0u : 1u) | (1u << 4) | ((uint32_t)p1len << 5);
        out[0] = (uint8_t)tag;
        out[1] = (uint8_t)(tag >> 8);
        out[2] = (uint8_t)(tag >> 16);
    }
    if (key) {
        out[3] = 0x9d;
        out[4] = 0x01;
        out[5] = 0x2a;
        out[6] = (uint8_t)e->w;
        out[7] = (uint8_t)(e->w >> 8);
        out[8] = (uint8_t)e->h;
        out[9] = (uint8_t)(e->h >> 8);
    }
    memcpy(out + hdr, e->p1, p1len);
    memmove(out + hdr + p1len, out + hdr + e->p1cap, p2len);

    e->have_ref = 1;
    e->stats.mbs = total;
    e->stats.skipped = e->nskip;
    e->stats.intra = e->nintra;
    e->stats.inter = total - e->nintra;
    e->stats.key = key;
    e->stats.changed = key ? total : e->nchanged;
    e->stats.moved = e->nmoved;
    e->hints = NULL; /* hints are for one frame */
    return hdr + p1len + p2len;
}

void vp8e_abandon(vp8e *e)
{
    /* The reconstruction is part-way into a frame the peer never saw. */
    e->have_ref = 0;
}

size_t vp8e_encode(vp8e *e, const vp8e_src *src, const uint8_t *dirty, int key,
                   uint8_t *out, size_t cap)
{
    if (!vp8e_begin(e, src, dirty, key, out, cap))
        return 0;
    vp8e_rows(e, e->mbh);
    return vp8e_end(e);
}
