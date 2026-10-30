/* What MSVC 4 leaves out, for code written against C99.
 *
 * Its _snprintf neither terminates a truncated string nor says how long the
 * whole would have been, and the core relies on both: this is the IRIX
 * port's (crustdesk-irix/src/compat.c) -- d i u x X c s p %, with '-', '0',
 * a width, a precision for strings, and the l, ll and z sizes. (The staging
 * script turns its long longs into __int64.)
 */
#include "compat.h"

#include <stdint.h>
#include <string.h>
#include "win.h"

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

/* GetTickCount: milliseconds since boot, only forward; it wraps after 49
 * days, which the agent's int32 differences of deadlines take in stride. */
uint32_t cdv_ms(void)
{
    return (uint32_t)GetTickCount();
}
