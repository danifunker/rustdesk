#include "dns.h"

#include <string.h>

int dns_parse_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    int parts = 0;
    while (parts < 4) {
        unsigned n = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            n = n * 10 + (unsigned)(*s++ - '0');
            digits++;
        }
        if (!digits || n > 255)
            return 0;
        v = v << 8 | n;
        parts++;
        if (parts < 4 && *s++ != '.')
            return 0;
    }
    if (*s)
        return 0;
    *ip = v;
    return 1;
}

size_t dns_query(uint8_t *out, size_t cap, uint16_t id, const char *name)
{
    size_t o = 12, nl = strlen(name);
    const char *p = name;
    if (cap < 12 + nl + 2 + 4)
        return 0;
    memset(out, 0, 12);
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)id;
    out[2] = 0x01; /* recursion desired */
    out[5] = 1;    /* one question */
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (!len || len > 63)
            return 0;
        out[o++] = (uint8_t)len;
        memcpy(out + o, p, len);
        o += len;
        p += len;
        if (*p == '.')
            p++;
    }
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 1; /* type A */
    out[o++] = 0;
    out[o++] = 1; /* class IN */
    return o;
}

/* Past a (possibly compressed) name. */
static size_t skip_name(const uint8_t *d, size_t n, size_t o)
{
    while (o < n) {
        uint8_t l = d[o];
        if (!l)
            return o + 1;
        if ((l & 0xC0) == 0xC0)
            return o + 2;
        o += 1 + l;
    }
    return n + 1;
}

int dns_answer(const uint8_t *d, size_t n, uint16_t id, uint32_t *ip)
{
    size_t o = 12;
    unsigned qd, an, i;
    if (n < 12 || ((unsigned)d[0] << 8 | d[1]) != id || !(d[2] & 0x80))
        return 0;
    if (d[3] & 0x0F)
        return -1; /* an error: no such name, server failure... */
    qd = (unsigned)d[4] << 8 | d[5];
    an = (unsigned)d[6] << 8 | d[7];
    for (i = 0; i < qd && o <= n; i++)
        o = skip_name(d, n, o) + 4;
    for (i = 0; i < an && o <= n; i++) {
        unsigned type, len;
        o = skip_name(d, n, o);
        if (o + 10 > n)
            break;
        type = (unsigned)d[o] << 8 | d[o + 1];
        len = (unsigned)d[o + 8] << 8 | d[o + 9];
        o += 10;
        if (o + len > n)
            break;
        if (type == 1 && len == 4) {
            *ip = (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
            return 1;
        }
        o += len;
    }
    return -1;
}
