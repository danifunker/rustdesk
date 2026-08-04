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
#include "vpx/vp8cx.h"
#include "vpx/vpx_image.h"

struct vpxenc {
    vpx_codec_ctx_t codec;
    vpx_image_t img;
    int width, height;
    unsigned char *out;   /* accumulated packets for the current frame */
    size_t out_len, out_cap;
    int out_key;
};

/* cpu_used: VP8 speed/quality dial. Negative is faster; -16 is the fastest the
 * encoder accepts and is what this hardware needs. */
struct vpxenc *vpxenc_new(int width, int height, int bitrate_kbps, int cpu_used)
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

    cfg.g_w = width;
    cfg.g_h = height;
    cfg.rc_target_bitrate = bitrate_kbps;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = 1000;          /* pts in milliseconds */
    cfg.g_error_resilient = 1;          /* survive a dropped packet */
    cfg.g_lag_in_frames = 0;            /* no lookahead: latency matters here */
    cfg.g_threads = 1;                  /* one session, one encode thread */
    cfg.rc_end_usage = VPX_CBR;
    cfg.kf_mode = VPX_KF_AUTO;
    cfg.rc_min_quantizer = 8;
    cfg.rc_max_quantizer = 56;          /* let quality drop rather than stall */
    cfg.rc_buf_sz = 1000;
    cfg.rc_buf_initial_sz = 500;
    cfg.rc_buf_optimal_sz = 600;

    if (vpx_codec_enc_init_ver(&e->codec, iface, &cfg, 0, VPX_ENCODER_ABI_VERSION)
            != VPX_CODEC_OK) {
        free(e);
        return NULL;
    }
    vpx_codec_control_(&e->codec, VP8E_SET_CPUUSED, cpu_used);
    vpx_codec_control_(&e->codec, VP8E_SET_STATIC_THRESHOLD, 1000);
    vpx_codec_control_(&e->codec, VP8E_SET_TOKEN_PARTITIONS, 0);

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

/* Encode one I420 frame. On success returns 0 and points *data/*len at an
 * internal buffer valid until the next call. */
int vpxenc_encode(struct vpxenc *e,
                  const unsigned char *y, const unsigned char *u, const unsigned char *v,
                  int ystride, int ustride, int vstride,
                  long long pts_ms, int force_key,
                  const unsigned char **data, size_t *len, int *is_key)
{
    if (!e || !y || !u || !v)
        return -1;

    e->img.planes[VPX_PLANE_Y] = (unsigned char *)y;
    e->img.planes[VPX_PLANE_U] = (unsigned char *)u;
    e->img.planes[VPX_PLANE_V] = (unsigned char *)v;
    e->img.stride[VPX_PLANE_Y] = ystride;
    e->img.stride[VPX_PLANE_U] = ustride;
    e->img.stride[VPX_PLANE_V] = vstride;

    vpx_enc_frame_flags_t flags = force_key ? VPX_EFLAG_FORCE_KF : 0;
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
    free(e->out);
    free(e);
}
