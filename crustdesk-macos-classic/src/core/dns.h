/* Just enough DNS to turn the ID server's and relay's names into addresses:
 * one A query, and the first A record of the answer (CNAMEs followed by
 * reading on). The platform sends and receives the datagrams.
 */
#ifndef DNS_H
#define DNS_H

#include <stddef.h>
#include <stdint.h>

/* A dotted quad, as a.b.c.d -> a<<24|b<<16|c<<8|d. */
int dns_parse_ip(const char *s, uint32_t *ip);

size_t dns_query(uint8_t *out, size_t cap, uint16_t id, const char *name);

/* 1 and *ip set if this is the answer to `id` and holds an A record; -1 if it
 * is that answer and holds none (or an error); 0 if it is not ours. */
int dns_answer(const uint8_t *d, size_t n, uint16_t id, uint32_t *ip);

#endif
