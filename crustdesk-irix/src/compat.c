/* What IRIX 5.3's libc and GCC's runtime leave out.
 *
 * snprintf/vsnprintf arrived in IRIX 6. This one knows the conversions the
 * agent and the shared core use -- d i u x X c s p %, with '-', '0', a width,
 * a precision for strings, and the l, ll and z sizes -- and always terminates.
 *
 * The libgcc helpers are the ones GCC calls for MIPS I, which has no
 * instructions for them. libgcc itself is not linked: the cross compiler's
 * copy is built for MIPS32r2 and would not run on an R3000.
 */
#include "compat.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    char *p;
    size_t left; /* room before the terminator */
    int total;
} sink;

static void put(sink *s, char c)
{
    if (s->left) {
        *s->p++ = c;
        s->left--;
    }
    s->total++;
}

static void pad(sink *s, char c, int n)
{
    while (n-- > 0)
        put(s, c);
}

int cdv_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    sink s;
    s.p = buf;
    s.left = n ? n - 1 : 0;
    s.total = 0;
    while (*fmt) {
        int left = 0, zero = 0, width = 0, prec = -1, size = 0, neg = 0, len, base = 10;
        int upper = 0;
        char tmp[24], *t;
        unsigned long long v;
        const char *str;
        if (*fmt != '%') {
            put(&s, *fmt++);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + *fmt++ - '0';
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + *fmt++ - '0';
        }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') {
            if (*fmt == 'l')
                size++;
            else if (*fmt == 'z')
                size = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's':
            str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            len = (int)strlen(str);
            if (prec >= 0 && len > prec)
                len = prec;
            if (!left)
                pad(&s, ' ', width - len);
            for (t = (char *)str; t < str + len; t++)
                put(&s, *t);
            if (left)
                pad(&s, ' ', width - len);
            fmt++;
            continue;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            if (!left)
                pad(&s, ' ', width - 1);
            put(&s, tmp[0]);
            if (left)
                pad(&s, ' ', width - 1);
            fmt++;
            continue;
        case '%':
            put(&s, '%');
            fmt++;
            continue;
        case 'd':
        case 'i': {
            long long sv = size >= 2 ? va_arg(ap, long long)
                         : size == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
            neg = sv < 0;
            v = neg ? (unsigned long long)-sv : (unsigned long long)sv;
            break;
        }
        case 'p':
            v = (unsigned long)va_arg(ap, void *);
            base = 16;
            put(&s, '0');
            put(&s, 'x');
            break;
        case 'X':
            upper = 1;
            /* fall through */
        case 'x':
            base = 16;
            /* fall through */
        case 'u':
            v = size >= 2 ? va_arg(ap, unsigned long long)
              : size == 1 ? (unsigned long long)va_arg(ap, unsigned long)
                          : (unsigned long long)va_arg(ap, unsigned int);
            break;
        default:
            put(&s, '%');
            if (*fmt)
                put(&s, *fmt++);
            continue;
        }
        fmt++;
        t = tmp + sizeof tmp;
        do {
            unsigned d = (unsigned)(v % (unsigned)base);
            *--t = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
            v /= (unsigned)base;
        } while (v);
        len = (int)(tmp + sizeof tmp - t) + neg;
        if (!left && !zero)
            pad(&s, ' ', width - len);
        if (neg)
            put(&s, '-');
        if (!left && zero)
            pad(&s, '0', width - len);
        while (t < tmp + sizeof tmp)
            put(&s, *t++);
        if (left)
            pad(&s, ' ', width - len);
    }
    if (n)
        *s.p = 0;
    return s.total;
}

int cdv_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = cdv_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- libgcc, the parts MIPS I needs ------------------------------------- */

uint32_t __bswapsi2(uint32_t x);
uint64_t __bswapdi2(uint64_t x);
int __clzsi2(uint32_t x);
int __ctzsi2(uint32_t x);

uint32_t __bswapsi2(uint32_t x)
{
    return x >> 24 | (x >> 8 & 0xff00) | (x << 8 & 0xff0000) | x << 24;
}

uint64_t __bswapdi2(uint64_t x)
{
    return (uint64_t)__bswapsi2((uint32_t)x) << 32 | __bswapsi2((uint32_t)(x >> 32));
}

int __clzsi2(uint32_t x)
{
    int n = 0;
    if (!(x & 0xffff0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xff000000u)) { n += 8; x <<= 8; }
    if (!(x & 0xf0000000u)) { n += 4; x <<= 4; }
    if (!(x & 0xc0000000u)) { n += 2; x <<= 2; }
    if (!(x & 0x80000000u)) { n += 1; }
    return n;
}

int __ctzsi2(uint32_t x)
{
    int n = 0;
    if (!(x & 0xffff)) { n += 16; x >>= 16; }
    if (!(x & 0xff)) { n += 8; x >>= 8; }
    if (!(x & 0xf)) { n += 4; x >>= 4; }
    if (!(x & 0x3)) { n += 2; x >>= 2; }
    if (!(x & 0x1)) { n += 1; }
    return n;
}

/* 64-bit division: MIPS I divides 32 bits at most. Shift and subtract. */
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem);
uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);
int64_t __divdi3(int64_t n, int64_t d);
int64_t __moddi3(int64_t n, int64_t d);

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    int i;
    if ((n >> 32) == 0 && (d >> 32) == 0) {
        uint32_t a = (uint32_t)n, b = (uint32_t)d;
        if (rem)
            *rem = a % b;
        return a / b;
    }
    for (i = 63; i >= 0; i--) {
        r = r << 1 | (n >> i & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, NULL); }

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) != (d < 0);
    uint64_t q = __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n,
                              d < 0 ? -(uint64_t)d : (uint64_t)d, NULL);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;
    __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
