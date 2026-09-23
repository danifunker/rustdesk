/* VP8-lite encoder. See vp8enc.h for what it does and does not use.
 *
 * Section numbers refer to RFC 6386. The tables in vp8tab.h are generated from
 * that document by tools/gen_vp8_tables.py.
 */
#include "vp8enc.h"
#include "vp8tab.h"

#include <string.h>

enum { DC_PRED = 0, V_PRED = 1, H_PRED = 2, TM_PRED = 3, B_PRED = 4 };

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
static void bw_put(bw *b, int prob, int bit)
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

static void put_extra(bw *b, const uint8_t *p, int bits, int v)
{
    int i;
    for (i = 0; i < bits; i++)
        bw_put(b, p[i], (v >> (bits - 1 - i)) & 1);
}

/* Code one block's tokens. `qc` is in zigzag order. Returns whether anything
 * nonzero was coded, which is the context its neighbours see. */
static int put_block(bw *b, const short *qc, int type, int first, int ctx)
{
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

struct vp8e {
    int w, h, mbw, mbh, q;
    int have_ref;
    uint8_t *ry, *ru, *rv;        /* reconstruction, macroblock aligned */
    int rys, ruvs;
    uint8_t *mbinfo;              /* mbw * mbh */
    uint8_t *exact;               /* mbw * mbh: reconstruction == source */
    uint8_t *above_ctx;           /* mbw * 9: token contexts, section 13.3 */
    uint8_t *p1;                  /* first partition is assembled here */
    size_t p1cap;
    int dq_y1[2], dq_y2[2], dq_uv[2];
    qstep qs_y1[2], qs_y2[2], qs_uv[2];
    vp8e_stats stats;

    /* The frame in progress, between vp8e_begin and vp8e_end. */
    int f_key, f_row;
    const uint8_t *f_dirty;
    vp8e_src f_src;
    uint8_t *f_out;
    size_t f_cap, f_hdr;
    bw tb;
    uint8_t left[9];
    int nskip, nintra, nchanged;

    /* Scratch for one macroblock. Here rather than on the stack, because on
     * the Mac the encoder runs at deferred-task time on whatever stack the
     * interrupted code had. */
    uint8_t sy[256], su[64], sv[64], py[256], pu[64], pv[64], cand[256], cu[64], cv[64];
    mbcoef qc;
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
           align4(256 + mbs * 12);
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
    vp8e_set_q(e, q);
    return e;
}

int vp8e_mb_cols(const vp8e *e) { return e->mbw; }
const uint8_t *vp8e_exact_map(const vp8e *e) { return e->exact; }
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

static int sad(const uint8_t *a, const uint8_t *b, int n)
{
    int s = 0, i;
    for (i = 0; i < n; i++) {
        int d = a[i] - b[i];
        s += d < 0 ? -d : d;
    }
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
            memcpy(dst + j * n, row + x, (size_t)n);
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
        memcpy(dst + j * n, rec + (y + j) * stride + x, (size_t)n);
}

/* ---- one macroblock ------------------------------------------------------- */




/* Residual of one 4x4 block at (bx, by) inside an n-wide block. */
static void residual4(const uint8_t *src, const uint8_t *pred, int n, int bx, int by, short *d)
{
    int i, j;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            int o = (by + j) * n + bx + i;
            d[j * 4 + i] = (short)(src[o] - pred[o]);
        }
}

/* Transform and quantise a macroblock; `qc` is filled in zigzag order.
 * Returns nonzero if any coefficient survived. */
static int quantise_mb(const vp8e *e, const uint8_t *sy, const uint8_t *py, const uint8_t *su,
                       const uint8_t *pu, const uint8_t *sv, const uint8_t *pv, mbcoef *qc)
{
    short d[16], c[16], dcs[16], w[16];
    int b, k, any = 0;
    for (b = 0; b < 16; b++) {
        residual4(sy, py, 16, (b & 3) * 4, (b >> 2) * 4, d);
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
        residual4(s, p, 8, (b & 1) * 4, ((b & 3) >> 1) * 4, d);
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
            memcpy(ry + j * e->rys, py + j * 16, 16);
        for (j = 0; j < 8; j++) {
            memcpy(ru + j * e->ruvs, pu + j * 8, 8);
            memcpy(rv + j * e->ruvs, pv + j * 8, 8);
        }
        return;
    }
    memset(y2, 0, sizeof y2);
    for (k = 0; k < 16; k++)
        y2[zigzag[k]] = (short)(qc->y2[k] * e->dq_y2[k ? 1 : 0]);
    iwalsh(y2, dcs);
    for (b = 0; b < 16; b++) {
        int bx = (b & 3) * 4, by = (b >> 2) * 4;
        memset(c, 0, sizeof c);
        c[0] = dcs[b];
        for (k = 1; k < 16; k++)
            c[zigzag[k]] = (short)(qc->y[b][k] * e->dq_y1[1]);
        idct_add(c, py + by * 16 + bx, 16, ry + by * e->rys + bx, e->rys);
    }
    for (b = 0; b < 8; b++) {
        const uint8_t *p = b < 4 ? pu : pv;
        uint8_t *r = b < 4 ? ru : rv;
        int bx = (b & 1) * 4, by = ((b & 3) >> 1) * 4;
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
                /* Section 16.3: every neighbour's vector is zero, so the census
                 * is just a weighted count of inter neighbours. Off-frame
                 * neighbours count as intra. */
                int cnt = 0;
                if (r > 0 && (mi[-e->mbw] & MB_INTER))
                    cnt += 2;
                if (c > 0 && (mi[-1] & MB_INTER))
                    cnt += 2;
                if (r > 0 && c > 0 && (mi[-e->mbw - 1] & MB_INTER))
                    cnt += 1;
                bw_put(b, 255, 0); /* mb_ref_frame_sel1 = LAST (prob_last) */
                bw_put(b, vp8_mode_contexts[cnt][0], 0); /* ZEROMV */
            } else {
                bw_tree(b, ymode_tree, ymode_prob, MB_YMODE(info));
                bw_tree(b, uv_mode_tree, uv_mode_prob, MB_UVMODE(info));
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
    e->nskip = e->nintra = e->nchanged = 0;
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
            int best, bestmode, inter = 0, m, coded, uvmode = DC_PRED;

            if (!key && e->f_dirty && !e->f_dirty[r * e->mbw + c]) {
                /* Unchanged: copy from the last frame, nothing coded. */
                *info = MB_INTER | MB_SKIP;
                memset(above, 0, 9);
                memset(left, 0, 9);
                e->nskip++;
                continue;
            }

            load_src(src->y, src->ystride, e->w, e->h, c * 16, r * 16, 16, sy);
            load_src(src->u, src->uvstride, cw, ch, c * 8, r * 8, 8, su);
            load_src(src->v, src->uvstride, cw, ch, c * 8, r * 8, 8, sv);

            /* Luma: the closest of the last frame and the intra modes whose
             * edges exist. Ties go to the last frame, which is cheaper. */
            best = 0x7fffffff;
            bestmode = DC_PRED;
            if (!key) {
                load_rec(e->ry, e->rys, c * 16, r * 16, 16, py);
                best = sad(sy, py, 256);
                inter = 1;
            }
            for (m = 0; m < 4 && best; m++) {
                int mode = ymodes[m], s;
                if ((mode == V_PRED && r == 0) || (mode == H_PRED && c == 0) ||
                    (mode == TM_PRED && (r == 0 || c == 0)))
                    continue;
                intra_pred(e->ry, e->rys, c * 16, r * 16, 16, mode, cand);
                s = sad(sy, cand, 256);
                if (s < best) {
                    best = s;
                    bestmode = mode;
                    inter = 0;
                    memcpy(py, cand, 256);
                }
            }

            if (inter) {
                load_rec(e->ru, e->ruvs, c * 8, r * 8, 8, pu);
                load_rec(e->rv, e->ruvs, c * 8, r * 8, 8, pv);
            } else {
                int bestuv = 0x7fffffff;
                for (m = 0; m < 4; m++) {
                    int mode = ymodes[m], s;
                    if ((mode == V_PRED && r == 0) || (mode == H_PRED && c == 0) ||
                        (mode == TM_PRED && (r == 0 || c == 0)))
                        continue;
                    intra_pred(e->ru, e->ruvs, c * 8, r * 8, 8, mode, e->cu);
                    intra_pred(e->rv, e->ruvs, c * 8, r * 8, 8, mode, e->cv);
                    s = sad(su, e->cu, 64) + sad(sv, e->cv, 64);
                    if (s < bestuv) {
                        bestuv = s;
                        uvmode = mode;
                        memcpy(pu, e->cu, 64);
                        memcpy(pv, e->cv, 64);
                    }
                }
            }

            coded = quantise_mb(e, sy, py, su, pu, sv, pv, qc);
            reconstruct_mb(e, c, r, py, pu, pv, qc, coded);
            {
                /* Did the peer get the source exactly? (Edge macroblocks
                 * compare their padding too, and may never say yes; the
                 * refresh just keeps visiting them.) */
                uint8_t *ex = e->exact + r * e->mbw + c;
                int j, same = 1;
                load_rec(e->ry, e->rys, c * 16, r * 16, 16, e->cand);
                same = !memcmp(e->cand, sy, 256);
                for (j = 0; j < 8 && same; j++)
                    same = !memcmp(e->ru + (r * 8 + j) * e->ruvs + c * 8, su + j * 8, 8) &&
                           !memcmp(e->rv + (r * 8 + j) * e->ruvs + c * 8, sv + j * 8, 8);
                *ex = (uint8_t)same;
            }
            *info = (uint8_t)((inter ? MB_INTER : 0) | (coded ? 0 : MB_SKIP) |
                              (bestmode << 1) | (uvmode << 4));
            /* Only an inter macroblock with nothing coded is a straight copy
             * of what the peer already has. */
            if (coded || !inter)
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
