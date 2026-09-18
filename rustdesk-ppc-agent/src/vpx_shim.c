/* Thin C wrapper around libvpx's VP8 encoder.
 *
 * Why a shim rather than direct FFI: vpx_codec_enc_cfg_t is a large struct whose
 * layout would have to be replicated exactly in Rust. Getting it subtly wrong on
 * a 32-bit big-endian target gives silent corruption, not a compile error. Here
 * the struct never crosses the language boundary -- only scalars and byte
 * buffers do -- so there is nothing to get wrong.
 *
 * It also gets compiled at the C toolchain's own optimisation level with
 * -maltivec, rather than the -O1 mrustc emits for Rust code.
 */
#include <stdlib.h>
#include <string.h>

#define VPX_CODEC_DISABLE_COMPAT 1
#include "vpx/vpx_codec.h"
#include "vpx/vpx_encoder.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "vpx/vp8cx.h"
#include "vpx/vpx_image.h"

struct vpxenc {
    vpx_codec_ctx_t codec;
    vpx_image_t img;
    int width, height;
    int last_ref_only;
    unsigned char *out;   /* accumulated packets for the current frame */
    size_t out_len, out_cap;
    int out_key;
    /* Active map: one byte per 16x16 macroblock, 1 = encode it, 0 = leave it
     * alone. Owned here rather than by the caller because libvpx copies it on
     * every set and the buffer has to outlive the control call anyway. */
    unsigned char *amap;
    int mb_rows, mb_cols;
    int amap_on;
};

/* cpu_used: VP8 speed/quality dial. Negative is faster; -16 is the fastest the
 * encoder accepts and is what this hardware needs.
 *
 * threads: decided by the caller from the processors actually online, never
 * assumed -- a single-processor G4 or G5 is as much a target as the dual G5
 * this was developed on. VP8 only threads across token partitions, so asking
 * for more than one thread means asking for partitions to match.
 *
 * The last three are the tuning knobs `--probe-display` sweeps; see the Rust
 * side's `Tune` for what each costs. They exist as parameters rather than
 * constants so the sweep measures the same code the session runs.
 */
struct vpxenc *vpxenc_new(int width, int height, int bitrate_kbps, int cpu_used, int threads,
                          int static_thresh, int last_ref_only, int error_resilient,
                          int profile, int min_q, int screen_content, int auto_keyframes)
{
    struct vpxenc *e;
    vpx_codec_enc_cfg_t cfg;
    vpx_codec_iface_t *iface = vpx_codec_vp8_cx();

    if (width <= 0 || height <= 0)
        return NULL;
    if (vpx_codec_enc_config_default(iface, &cfg, 0) != VPX_CODEC_OK)
        return NULL;

    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->width = width;
    e->height = height;
    e->last_ref_only = last_ref_only;

    /* VP8 bitstream profile, which on the encoding side is a set of cost
     * decisions rather than a feature level -- every decoder must handle all
     * four, so this is free to pick from (see vp8_setup_version):
     *
     *   0  normal loop filter, six-tap sub-pixel motion compensation
     *   1  simple loop filter, bilinear MC
     *   2  NO LOOP FILTER, bilinear MC
     *   3  NO LOOP FILTER, simple filter, FULL-PIXEL motion only
     *
     * The loop filter is a whole-frame pass whose job is hiding block edges in
     * natural video. On a desktop, where most macroblocks are coded as skip and
     * the content is text on flat colour, it costs a pass over every pixel to
     * slightly blur the thing the user is trying to read. Profiles 2 and 3 set
     * cm->no_lpf and the pass -- and the filter-level search in front of it --
     * disappears entirely. */
    if (profile >= 0 && profile <= 3)
        cfg.g_profile = (unsigned int)profile;

    cfg.g_w = width;
    cfg.g_h = height;
    cfg.rc_target_bitrate = bitrate_kbps;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = 1000;          /* pts in milliseconds */
    /* Error resilience buys survival of a dropped packet, which is worth
     * nothing over a TCP session that cannot drop one -- so turning it off
     * looked like free quality, since it re-enables the backward entropy
     * update. Measured on the G5 it is neither: no faster, and 9% *more* bytes
     * per frame. Left on, and left switchable, because that result is odd
     * enough to be worth re-checking on other hardware. */
    cfg.g_error_resilient = error_resilient ? 1 : 0;
    cfg.g_lag_in_frames = 0;            /* no lookahead: latency matters here */
    if (threads < 1)
        threads = 1;
    if (threads > 4)
        threads = 4;
    cfg.g_threads = threads;
    cfg.rc_end_usage = VPX_CBR;
    /* Key frames only when asked for, unless the caller wants libvpx's own as
     * well -- see `Tune::auto_keyframes`. */
    cfg.kf_mode = auto_keyframes ? VPX_KF_AUTO : VPX_KF_DISABLED;
    /* The floor on quality, and so a floor on how many coefficients there are
     * to transform, quantise and tokenise. 8 is near-lossless and was chosen on
     * a machine where the encoder was not the bottleneck. */
    cfg.rc_min_quantizer = (min_q >= 0 && min_q <= 63) ? (unsigned int)min_q : 8;
    cfg.rc_max_quantizer = 56;          /* let quality drop rather than stall */
    if (cfg.rc_max_quantizer < cfg.rc_min_quantizer)
        cfg.rc_max_quantizer = cfg.rc_min_quantizer;
    cfg.rc_buf_sz = 1000;
    cfg.rc_buf_initial_sz = 500;
    cfg.rc_buf_optimal_sz = 600;

    if (vpx_codec_enc_init_ver(&e->codec, iface, &cfg, 0, VPX_ENCODER_ABI_VERSION)
            != VPX_CODEC_OK) {
        free(e);
        return NULL;
    }
    vpx_codec_control_(&e->codec, VP8E_SET_CPUUSED, cpu_used);
    /* Tell VP8 this is a desktop, not a camera.
     *
     * Three things in the realtime mode picker are conditioned on it and all
     * three are wrong for screen content: the dot-artifact check (a camera
     * denoising heuristic), the skin-map lookup (there is no skin on a
     * desktop), and a ZEROMV rate-distortion bias tuned for natural video.
     * Mode 2 additionally keeps a golden frame updated, which is what a mostly
     * static screen wants. Costs nothing to ask for; see the Rust `Tune`. */
    if (screen_content >= 0 && screen_content <= 2)
        vpx_codec_control_(&e->codec, VP8E_SET_SCREEN_CONTENT_MODE,
                           (unsigned int)screen_content);
    /* Below this much residual error a macroblock is declared unchanged and
     * coded as a skip, which is the single cheapest thing that can happen to
     * it. A desktop is mostly *exactly* static, so the threshold only has to be
     * high enough to swallow the odd blend or antialiased edge -- and this is
     * the one knob here that measurably pays. See the Rust `Tune` for the
     * numbers and for why the default is 15000 rather than higher. */
    vpx_codec_control_(&e->codec, VP8E_SET_STATIC_THRESHOLD, static_thresh);
    /* One partition per thread, rounded down to the power of two VP8 wants:
     * without this g_threads has nothing to divide the work along. */
    vpx_codec_control_(&e->codec, VP8E_SET_TOKEN_PARTITIONS, threads >= 4 ? 2 : (threads >= 2 ? 1 : 0));

    e->mb_rows = (height + 15) / 16;
    e->mb_cols = (width + 15) / 16;
    e->amap = calloc((size_t)e->mb_rows * (size_t)e->mb_cols, 1);
    if (!e->amap) {
        vpx_codec_destroy(&e->codec);
        free(e);
        return NULL;
    }

    /* Wrap caller-supplied planes rather than allocating: vpx_img_wrap with a
     * NULL data pointer sets up the descriptor only, and encode() fills in the
     * plane pointers. Saves a full frame copy per encode. */
    if (!vpx_img_wrap(&e->img, VPX_IMG_FMT_I420, width, height, 1, (unsigned char *)1)) {
        vpx_codec_destroy(&e->codec);
        free(e);
        return NULL;
    }
    return e;
}

/* --------------------------------------------------------------------------
 * Active map: tell the encoder which macroblocks are worth looking at.
 *
 * VP8's cost is per macroblock and almost independent of what is in one, so an
 * ordinary desktop frame -- a cursor, a line of text, a blinking caret --
 * pays for the entire screen to find out that nothing else moved. The server
 * here reports exact damage rectangles, so that search is answerable for free.
 *
 * `VP8E_SET_ACTIVEMAP` makes the realtime mode picker exit immediately on an
 * inactive macroblock (evaluate_inter_mode in vp8/encoder/pickinter.c), which
 * codes it as a skip and costs nothing but the loop over it.
 *
 * Two things are worth knowing before relying on it:
 *   - It applies to inter frames. A keyframe re-codes everything by definition,
 *     so vpxenc_encode turns the map off around a forced keyframe rather than
 *     leaving libvpx to decide what a partial keyframe means.
 *   - Anything left inactive keeps the previous frame's pixels *for ever*. It
 *     is only safe on top of a damage report that cannot miss a change, which
 *     is what SGI-SCREEN-CAPTURE gives us and what the Mac's sampled probe
 *     explicitly does not.
 * -------------------------------------------------------------------------- */

int vpxenc_mb_rows(const struct vpxenc *e) { return e ? e->mb_rows : 0; }
int vpxenc_mb_cols(const struct vpxenc *e) { return e ? e->mb_cols : 0; }

/* Start a fresh map with every macroblock inactive. */
void vpxenc_amap_clear(struct vpxenc *e)
{
    if (!e || !e->amap) return;
    memset(e->amap, 0, (size_t)e->mb_rows * (size_t)e->mb_cols);
    e->amap_on = 1;
}

/* Mark the macroblocks a destination-pixel rectangle touches. */
void vpxenc_amap_rect(struct vpxenc *e, int x, int y, int w, int h)
{
    int r0, r1, c0, c1, r;

    if (!e || !e->amap || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;

    c0 = x / 16;
    r0 = y / 16;
    c1 = (x + w - 1) / 16;
    r1 = (y + h - 1) / 16;
    if (c1 >= e->mb_cols) c1 = e->mb_cols - 1;
    if (r1 >= e->mb_rows) r1 = e->mb_rows - 1;
    if (c0 > c1 || r0 > r1) return;

    for (r = r0; r <= r1; r++)
        memset(e->amap + (size_t)r * e->mb_cols + c0, 1, (size_t)(c1 - c0 + 1));
}

/* Stop restricting: every macroblock is a candidate again. */
void vpxenc_amap_off(struct vpxenc *e)
{
    if (!e) return;
    e->amap_on = 0;
}

/* How many macroblocks the current map would encode. For instrumentation --
 * "12 of 1280" is the number that says whether any of this is working. */
int vpxenc_amap_active(const struct vpxenc *e)
{
    int i, n = 0, total;
    if (!e || !e->amap || !e->amap_on) return e ? e->mb_rows * e->mb_cols : 0;
    total = e->mb_rows * e->mb_cols;
    for (i = 0; i < total; i++)
        if (e->amap[i]) n++;
    return n;
}

static int out_reserve(struct vpxenc *e, size_t need)
{
    if (e->out_cap >= need)
        return 1;
    size_t cap = e->out_cap ? e->out_cap : 65536;
    while (cap < need)
        cap *= 2;
    unsigned char *p = realloc(e->out, cap);
    if (!p)
        return 0;
    e->out = p;
    e->out_cap = cap;
    return 1;
}

/* Encode one I420 frame. On success returns 0 and points data/len at an
 * internal buffer valid until the next call. */
int vpxenc_encode(struct vpxenc *e,
                  const unsigned char *y, const unsigned char *u, const unsigned char *v,
                  int ystride, int ustride, int vstride,
                  long long pts_ms, int force_key,
                  const unsigned char **data, size_t *len, int *is_key)
{
    vpx_enc_frame_flags_t flags;

    if (!e || !y || !u || !v)
        return -1;

    e->img.planes[VPX_PLANE_Y] = (unsigned char *)y;
    e->img.planes[VPX_PLANE_U] = (unsigned char *)u;
    e->img.planes[VPX_PLANE_V] = (unsigned char *)v;
    e->img.stride[VPX_PLANE_Y] = ystride;
    e->img.stride[VPX_PLANE_U] = ustride;
    e->img.stride[VPX_PLANE_V] = vstride;

    /* A keyframe codes every macroblock by definition, so a map that says
     * otherwise is at best ignored and at worst a partial picture the peer can
     * never correct. NULL turns the restriction off for that frame; the map
     * itself is kept, and the next inter frame gets it back. */
    {
        vpx_active_map_t m;
        m.active_map = (e->amap_on && !force_key) ? e->amap : NULL;
        m.rows = (unsigned int)e->mb_rows;
        m.cols = (unsigned int)e->mb_cols;
        vpx_codec_control_(&e->codec, VP8E_SET_ACTIVEMAP, &m);
    }

    flags = force_key ? VPX_EFLAG_FORCE_KF : 0;
    /* Restrict prediction to the previous frame, on the theory that VP8 was
     * searching the golden and altref buffers too and that a desktop block
     * matches the frame before it or nothing at all. Measured on the G5: no
     * difference whatever, because at cpu_used = -16 the fast mode picker was
     * evidently not searching them anyway. Kept as a knob, not as a win.
     *
     * Only on inter frames: a keyframe refreshes every reference by
     * definition, so telling it not to is at best ignored. */
    if (e->last_ref_only && !force_key)
        flags |= VP8_EFLAG_NO_REF_GF | VP8_EFLAG_NO_REF_ARF
               | VP8_EFLAG_NO_UPD_GF | VP8_EFLAG_NO_UPD_ARF;
    /* Realtime deadline: bounded encode time, which is the whole point here. */
    if (vpx_codec_encode(&e->codec, &e->img, pts_ms, 1, flags, VPX_DL_REALTIME)
            != VPX_CODEC_OK)
        return -2;

    e->out_len = 0;
    e->out_key = 0;

    vpx_codec_iter_t iter = NULL;
    const vpx_codec_cx_pkt_t *pkt;
    while ((pkt = vpx_codec_get_cx_data(&e->codec, &iter)) != NULL) {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT)
            continue;
        /* Concatenating is correct for VP8 with lag_in_frames=0, which emits one
         * frame packet per input; the loop is here so extra packets are appended
         * rather than silently dropped. */
        if (!out_reserve(e, e->out_len + pkt->data.frame.sz))
            return -3;
        memcpy(e->out + e->out_len, pkt->data.frame.buf, pkt->data.frame.sz);
        e->out_len += pkt->data.frame.sz;
        if (pkt->data.frame.flags & VPX_FRAME_IS_KEY)
            e->out_key = 1;
    }

    *data = e->out;
    *len = e->out_len;
    *is_key = e->out_key;
    return 0;
}

void vpxenc_free(struct vpxenc *e)
{
    if (!e)
        return;
    vpx_img_free(&e->img);
    vpx_codec_destroy(&e->codec);
    free(e->amap);
    free(e->out);
    free(e);
}

/* --------------------------------------------------------------------------
 * A VP8 decoder, for checking what the peer actually receives.
 *
 * The encoder has been verified against its own round trip and the converter
 * against a reference implementation, and neither answers the question that
 * matters: does the picture arriving at the other end look right? A red and
 * blue swap survives both of those checks happily -- it did, for two sessions,
 * because the two halves agreed with each other and both were wrong.
 *
 * So `testpeer` decodes the frames it is sent and writes one out. This is also
 * the check any change to the encoder's settings needs: profile 3 drops the
 * loop filter and sub-pixel motion compensation, and "still decodes" is not the
 * same claim as "still looks like the screen".
 *
 * Output is I420 planes, which is what the decoder produces; the caller does
 * the colour conversion so the same arithmetic is exercised in both directions.
 * -------------------------------------------------------------------------- */

struct vpxdec {
    vpx_codec_ctx_t codec;
    int have_frame;
    int w, h;
};

struct vpxdec *vpxdec_new(void)
{
    struct vpxdec *d = calloc(1, sizeof(*d));
    vpx_codec_dec_cfg_t cfg;
    if (!d)
        return NULL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.threads = 1;
    if (vpx_codec_dec_init_ver(&d->codec, vpx_codec_vp8_dx(), &cfg, 0,
                               VPX_DECODER_ABI_VERSION) != VPX_CODEC_OK) {
        free(d);
        return NULL;
    }
    return d;
}

/* Decode one frame and copy its planes out. Returns 0 on success and fills in
 * the geometry; -1 if the bitstream was refused, -2 if the caller's buffers are
 * too small (in which case w and h still report what was needed). */
int vpxdec_decode(struct vpxdec *d, const unsigned char *data, size_t len,
                  unsigned char *y, unsigned char *u, unsigned char *v,
                  size_t ycap, size_t uvcap, int *w, int *h)
{
    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img;
    int row;

    if (!d || !data || !len)
        return -1;
    if (vpx_codec_decode(&d->codec, data, (unsigned int)len, NULL, 0) != VPX_CODEC_OK)
        return -1;
    img = vpx_codec_get_frame(&d->codec, &iter);
    if (!img)
        return -1;

    *w = (int)img->d_w;
    *h = (int)img->d_h;
    if (ycap < (size_t)img->d_w * img->d_h ||
        uvcap < (size_t)((img->d_w + 1) / 2) * ((img->d_h + 1) / 2))
        return -2;

    for (row = 0; row < (int)img->d_h; row++)
        memcpy(y + (size_t)row * img->d_w,
               img->planes[VPX_PLANE_Y] + (size_t)row * img->stride[VPX_PLANE_Y],
               img->d_w);
    for (row = 0; row < (int)(img->d_h + 1) / 2; row++) {
        memcpy(u + (size_t)row * ((img->d_w + 1) / 2),
               img->planes[VPX_PLANE_U] + (size_t)row * img->stride[VPX_PLANE_U],
               (img->d_w + 1) / 2);
        memcpy(v + (size_t)row * ((img->d_w + 1) / 2),
               img->planes[VPX_PLANE_V] + (size_t)row * img->stride[VPX_PLANE_V],
               (img->d_w + 1) / 2);
    }
    d->have_frame = 1;
    return 0;
}

void vpxdec_free(struct vpxdec *d)
{
    if (!d)
        return;
    vpx_codec_destroy(&d->codec);
    free(d);
}
