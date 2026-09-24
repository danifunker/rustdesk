#include "console.h"

#include "macroman.h"
#include "session.h"

#include <string.h>

int console_parse_url(const char *s, console_url *u)
{
    const char *h, *e, *colon;
    size_t n;
    memset(u, 0, sizeof *u);
    u->secure = 1;
    if (!strncmp(s, "https://", 8)) {
        h = s + 8;
    } else if (!strncmp(s, "http://", 7)) {
        h = s + 7;
        u->secure = 0;
    } else if (strstr(s, "://")) {
        return 0;
    } else {
        h = s;
    }
    e = h + strcspn(h, "/");
    colon = memchr(h, ':', (size_t)(e - h));
    n = (size_t)((colon ? colon : e) - h);
    if (!n || n >= sizeof u->host)
        return 0;
    memcpy(u->host, h, n);
    u->port = u->secure ? 443 : 80;
    if (colon) {
        long p = 0;
        const char *d;
        for (d = colon + 1; d < e; d++) {
            if (*d < '0' || *d > '9')
                return 0;
            p = p * 10 + (*d - '0');
        }
        if (p < 1 || p > 65535)
            return 0;
        u->port = (uint16_t)p;
    }
    n = strlen(e);
    while (n && e[n - 1] == '/')
        n--; /* "https://host/" is the same console as "https://host" */
    if (n >= sizeof u->prefix)
        return 0;
    memcpy(u->prefix, e, n);
    return 1;
}

void console_base64(const uint8_t *in, size_t n, char *out)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        *out++ = a[v >> 18];
        *out++ = a[(v >> 12) & 63];
        *out++ = a[(v >> 6) & 63];
        *out++ = a[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        *out++ = a[v >> 18];
        *out++ = a[(v >> 12) & 63];
        *out++ = '=';
        *out++ = '=';
    } else if (n - i == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
        *out++ = a[v >> 18];
        *out++ = a[(v >> 12) & 63];
        *out++ = a[(v >> 6) & 63];
        *out++ = '=';
    }
    *out = 0;
}

/* JSON building into a fixed buffer; `over` once anything did not fit. */
typedef struct {
    char *p, *end;
    int over;
} jw;

static void raw(jw *w, const char *s, size_t n)
{
    if (w->over || (size_t)(w->end - w->p) < n) {
        w->over = 1;
        return;
    }
    memcpy(w->p, s, n);
    w->p += n;
}

/* A string field, from Mac Roman. */
static void field(jw *w, const char *key, const char *mac, int first)
{
    uint8_t u8[256];
    size_t n = macroman_to_utf8((const uint8_t *)mac, strlen(mac), u8, sizeof u8), i;
    if (!first)
        raw(w, ",", 1);
    raw(w, "\"", 1);
    raw(w, key, strlen(key));
    raw(w, "\":\"", 3);
    for (i = 0; i < n; i++) {
        uint8_t c = u8[i];
        if (c == '"' || c == '\\') {
            char esc[2] = { '\\', (char)c };
            raw(w, esc, 2);
        } else if (c < 0x20) {
            static const char hx[] = "0123456789abcdef";
            char esc[6] = { '\\', 'u', '0', '0', hx[c >> 4], hx[c & 15] };
            raw(w, esc, 6);
        } else {
            raw(w, (const char *)&c, 1);
        }
    }
    raw(w, "\"", 1);
}

static size_t done(jw *w, char *out)
{
    raw(w, "}", 1);
    raw(w, "", 1); /* room for the terminator */
    if (w->over)
        return 0;
    return (size_t)(w->p - out - 1);
}

size_t console_sysinfo_json(char *out, size_t cap, const char *id, const uint8_t uuid[16],
                            const char *cpu, const char *memory, const char *os,
                            const char *hostname, const char *username)
{
    jw w = { out, out + cap, 0 };
    char u[32];
    console_base64(uuid, 16, u);
    raw(&w, "{", 1);
    field(&w, "id", id, 1);
    field(&w, "uuid", u, 0);
    field(&w, "cpu", cpu, 0);
    field(&w, "memory", memory, 0);
    field(&w, "os", os, 0);
    /* Never empty: a console asks again, every beat, for a row whose
     * hostname is blank. */
    field(&w, "hostname", hostname[0] ? hostname : "Macintosh", 0);
    field(&w, "username", username, 0);
    field(&w, "version", REPORTED_VERSION, 0);
    return done(&w, out);
}

size_t console_heartbeat_json(char *out, size_t cap, const char *id, const uint8_t uuid[16])
{
    /* No "conns": the console reads its absence as "nothing live here", and a
     * wrong list would close sessions. */
    jw w = { out, out + cap, 0 };
    char u[32];
    console_base64(uuid, 16, u);
    raw(&w, "{", 1);
    field(&w, "id", id, 1);
    field(&w, "uuid", u, 0);
    return done(&w, out);
}

/* Skip a JSON string starting at the opening quote; returns past the close. */
static const char *skip_string(const char *p)
{
    for (p++; *p && *p != '"'; p++)
        if (*p == '\\' && p[1])
            p++;
    return *p ? p + 1 : p;
}

int console_wants_sysinfo(const char *j)
{
    int depth = 0, expect_key = 0;
    const char *p = j;
    while (*p) {
        char c = *p;
        if (c == '"') {
            const char *s = p, *e = skip_string(p);
            if (depth == 1 && expect_key) {
                int match = e - s == 9 && !memcmp(s, "\"sysinfo\"", 9);
                p = e;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                    p++;
                if (*p != ':')
                    return 0;
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                    p++;
                if (match)
                    return !(!strncmp(p, "false", 5) || !strncmp(p, "null", 4) ||
                             (p[0] == '0' && !(p[1] >= '0' && p[1] <= '9') && p[1] != '.') ||
                             !strncmp(p, "\"\"", 2));
                expect_key = 0;
                continue;
            }
            p = e;
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
            expect_key = c == '{' && depth == 1;
        } else if (c == '}' || c == ']') {
            depth--;
        } else if (c == ',' && depth == 1) {
            expect_key = 1;
        }
        p++;
    }
    return 0;
}
