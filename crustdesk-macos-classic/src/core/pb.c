#include "pb.h"

#include <string.h>

size_t pb_varint_size(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

size_t pb_put_varint(uint8_t *p, uint64_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        p[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

void pbw_init(pbw *w, uint8_t *buf, size_t cap)
{
    w->p = buf;
    w->end = buf + cap;
    w->overflow = 0;
    w->depth = 0;
}

size_t pbw_len(const pbw *w, const uint8_t *buf)
{
    return (size_t)(w->p - buf);
}

static int room(pbw *w, size_t n)
{
    if (w->overflow || (size_t)(w->end - w->p) < n) {
        w->overflow = 1;
        return 0;
    }
    return 1;
}

static void raw_varint(pbw *w, uint64_t v)
{
    if (room(w, 10))
        w->p += pb_put_varint(w->p, v);
}

static void tag(pbw *w, int field, int wire)
{
    raw_varint(w, ((uint64_t)field << 3) | (uint64_t)wire);
}

void pbw_varint(pbw *w, int field, uint64_t v)
{
    /* proto3 leaves defaults off the wire */
    if (!v)
        return;
    tag(w, field, PB_VARINT);
    raw_varint(w, v);
}

void pbw_sint(pbw *w, int field, int32_t v)
{
    pbw_varint(w, field, (uint32_t)((v << 1) ^ (v >> 31)));
}

void pbw_bool(pbw *w, int field, int v)
{
    pbw_varint(w, field, v ? 1 : 0);
}

void pbw_bytes(pbw *w, int field, const void *data, size_t n)
{
    if (!n)
        return;
    tag(w, field, PB_LEN);
    raw_varint(w, n);
    if (room(w, n)) {
        memcpy(w->p, data, n);
        w->p += n;
    }
}

void pbw_string(pbw *w, int field, const char *s)
{
    pbw_bytes(w, field, s, strlen(s));
}

void pbw_begin(pbw *w, int field)
{
    tag(w, field, PB_LEN);
    if (w->depth >= 8 || !room(w, 1)) {
        w->overflow = 1;
        return;
    }
    w->open[w->depth++] = w->p;
    *w->p++ = 0;
}

void pbw_end(pbw *w)
{
    uint8_t *lenp, *body;
    size_t n, extra;
    if (w->overflow || !w->depth)
        return;
    lenp = w->open[--w->depth];
    body = lenp + 1;
    n = (size_t)(w->p - body);
    extra = pb_varint_size(n) - 1;
    if (extra) {
        if (!room(w, extra))
            return;
        memmove(body + extra, body, n);
        w->p += extra;
    }
    pb_put_varint(lenp, n);
}

void pbr_init(pbr *r, const uint8_t *buf, size_t n)
{
    r->p = buf;
    r->end = buf + n;
    r->error = 0;
}

static int get_varint(pbr *r, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (r->p < r->end && shift < 64) {
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 1;
        }
        shift += 7;
    }
    r->error = 1;
    return 0;
}

int pbr_next(pbr *r)
{
    uint64_t key;
    if (r->error || r->p >= r->end)
        return 0;
    if (!get_varint(r, &key))
        return 0;
    r->field = (int)(key >> 3);
    r->wire = (int)(key & 7);
    switch (r->wire) {
    case PB_VARINT:
        return get_varint(r, &r->v);
    case PB_I64:
    case PB_I32: {
        size_t n = r->wire == PB_I64 ? 8 : 4, i;
        if ((size_t)(r->end - r->p) < n) {
            r->error = 1;
            return 0;
        }
        r->v = 0;
        for (i = 0; i < n; i++) /* little-endian on the wire, whatever the host */
            r->v |= (uint64_t)r->p[i] << (8 * i);
        r->p += n;
        return 1;
    }
    case PB_LEN: {
        uint64_t n;
        if (!get_varint(r, &n))
            return 0;
        if (n > (uint64_t)(r->end - r->p)) {
            r->error = 1;
            return 0;
        }
        r->data = r->p;
        r->len = (size_t)n;
        r->p += n;
        return 1;
    }
    default:
        r->error = 1; /* groups are long dead */
        return 0;
    }
}

void pbr_sub(const pbr *r, pbr *sub)
{
    pbr_init(sub, r->data, r->len);
}

int32_t pb_unzigzag32(uint64_t v)
{
    uint32_t u = (uint32_t)v;
    return (int32_t)((u >> 1) ^ (0u - (u & 1)));
}
