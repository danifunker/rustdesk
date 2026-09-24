/* Just enough protobuf for the RustDesk wire format.
 *
 * Writing: messages are built front to back in a caller's buffer. A nested
 * message reserves one length byte and widens it at the end if the body
 * outgrew 127 bytes -- rare for everything except video, which the session
 * builds by hand around the encoder's output instead.
 *
 * Reading: a cursor over a buffer that hands back one field at a time.
 * Unknown fields are skipped, as protobuf requires; that is how a newer
 * client's extra fields pass through harmlessly.
 */
#ifndef PB_H
#define PB_H

#include <stddef.h>
#include <stdint.h>

enum { PB_VARINT = 0, PB_I64 = 1, PB_LEN = 2, PB_I32 = 5 };

typedef struct pbw {
    uint8_t *p, *end;
    int overflow;
    int depth;
    uint8_t *open[8]; /* where each open nested message's length byte is */
} pbw;

void pbw_init(pbw *w, uint8_t *buf, size_t cap);
size_t pbw_len(const pbw *w, const uint8_t *buf);
void pbw_varint(pbw *w, int field, uint64_t v);
void pbw_varint_always(pbw *w, int field, uint64_t v); /* a oneof member: even 0 */
void pbw_sint(pbw *w, int field, int32_t v); /* zigzag, for sint32 */
void pbw_bool(pbw *w, int field, int v);
void pbw_bytes(pbw *w, int field, const void *data, size_t n);
void pbw_string(pbw *w, int field, const char *s);
void pbw_begin(pbw *w, int field);
void pbw_end(pbw *w);

/* Raw encoders, for building headers by hand. Return bytes written. */
size_t pb_put_varint(uint8_t *p, uint64_t v);
size_t pb_varint_size(uint64_t v);

typedef struct {
    const uint8_t *p, *end;
    int error;
    /* the current field */
    int field, wire;
    uint64_t v;            /* PB_VARINT, PB_I32, PB_I64 */
    const uint8_t *data;   /* PB_LEN */
    size_t len;
} pbr;

void pbr_init(pbr *r, const uint8_t *buf, size_t n);
/* Advance to the next field. Returns 0 at the end or on malformed input
 * (r->error says which). */
int pbr_next(pbr *r);
/* A reader over the current PB_LEN field's contents. */
void pbr_sub(const pbr *r, pbr *sub);
int32_t pb_unzigzag32(uint64_t v);

#endif
