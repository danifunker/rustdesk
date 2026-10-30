#include "text.h"

size_t utf8_to_utf16(const char *s, size_t n, WCHAR *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    size_t o = 0;
    if (!cap)
        return 0;
    while (p < e && o + 1 < cap) {
        unsigned long c = *p++;
        int more = 0;
        if (c >= 0xf0 && c < 0xf8) {
            c &= 7;
            more = 3;
        } else if (c >= 0xe0) {
            c &= 15;
            more = 2;
        } else if (c >= 0xc0) {
            c &= 31;
            more = 1;
        } else if (c >= 0x80) {
            c = 0xfffd;
        }
        while (more-- > 0) {
            if (p >= e || (*p & 0xc0) != 0x80) {
                c = 0xfffd;
                break;
            }
            c = c << 6 | (*p++ & 0x3f);
        }
        if (c > 0xffff) {
            if (o + 2 >= cap)
                break;
            c -= 0x10000;
            out[o++] = (WCHAR)(0xd800 | (c >> 10));
            out[o++] = (WCHAR)(0xdc00 | (c & 0x3ff));
        } else {
            out[o++] = (WCHAR)c;
        }
    }
    out[o] = 0;
    return o;
}

size_t utf16_to_utf8(const WCHAR *w, size_t n, char *out, size_t cap)
{
    size_t i, o = 0;
    if (!cap)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned long c = w[i];
        int len;
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (w[i + 1] - 0xdc00);
            i++;
        }
        len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (o + len >= cap)
            break;
        switch (len) {
        case 1:
            out[o++] = (char)c;
            break;
        case 2:
            out[o++] = (char)(0xc0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3f));
            break;
        case 3:
            out[o++] = (char)(0xe0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
            break;
        default:
            out[o++] = (char)(0xf0 | c >> 18);
            out[o++] = (char)(0x80 | (c >> 12 & 0x3f));
            out[o++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[o] = 0;
    return o;
}
