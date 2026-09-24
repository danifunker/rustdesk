/* The ID server's side of the protocol (hbbs/hbbr), with no I/O of its own --
 * rustdesk-ppc-agent's rendezvous.rs and lan.rs, as a state machine.
 *
 * Registration is UDP to hbbs: RegisterPeer every 15 s (every 3 s until it is
 * answered), RegisterPk when hbbs asks for the key. What comes back tells the
 * agent a peer wants it:
 *
 *   PunchHole     -> RDV_RELAY, initiating: tell hbbs over TCP which relay and
 *                    uuid (RelayResponse with our id), then join hbbr.
 *   RequestRelay  -> RDV_RELAY: hbbs chose the uuid; tell it we are coming,
 *                    then join hbbr with that uuid.
 *   FetchLocalAddr-> RDV_LOCAL: the peer is on our network; tell hbbs over
 *                    TCP where to reach us (LocalAddr), and wait for it.
 *
 * Every one of those sessions is encrypted (cdv_start(..., secure=1)); a
 * direct-IP one is not. The platform does the sockets: this builds and reads
 * the messages.
 */
#ifndef RDV_H
#define RDV_H

#include <stddef.h>
#include <stdint.h>

#define RDV_PORT 21116   /* hbbs */
#define RELAY_PORT 21117 /* hbbr */
#define LAN_PORT 21119   /* discovery broadcasts */

typedef struct {
    /* configuration */
    char id[16];
    uint8_t uuid[16];
    uint8_t pk[32];          /* Ed25519 public key */
    char key[64];            /* the server's key, for a relay started with -k */
    char relay[64];          /* override for the relay hbbs names; "" to follow it */
    /* state */
    uint32_t sent_at, answered_at;
    int awaiting, registered, refused;
    uint32_t dedup_ip, dedup_at;
    uint16_t dedup_port;
    uint32_t mangle_clock;   /* anything that moves: obfuscation only */
} cdv_rdv;

enum { RDV_NONE, RDV_RELAY, RDV_LOCAL, RDV_REGISTERED, RDV_REFUSED };

typedef struct {
    int kind;
    int initiate;            /* PunchHole: we pick the uuid and say who we are */
    int secure;
    char relay[64];          /* host[:port] of hbbr */
    char uuid[48];
    uint8_t socket_addr[16]; /* the peer, as hbbs mangled it: echoed back */
    size_t socket_addr_len;
    uint32_t peer_ip;        /* decoded, for logs */
    uint16_t peer_port;
    int refuse_code;         /* RDV_REFUSED: RegisterPkResponse.result */
} rdv_action;

void rdv_init(cdv_rdv *r);

/* A datagram for hbbs, if one is due (0 if not). */
size_t rdv_tick(cdv_rdv *r, uint32_t now_ms, uint8_t *out, size_t cap);

/* A datagram from hbbs. May produce an action, and a reply datagram (RegisterPk)
 * in `reply` (*replylen 0 if none). Returns the action kind. */
int rdv_input(cdv_rdv *r, const uint8_t *d, size_t n, uint32_t now_ms, rdv_action *a,
              uint8_t *reply, size_t cap, size_t *replylen);

/* Framed TCP messages. To hbbs: where the session will be. To hbbr: which
 * session this connection belongs to. */
size_t rdv_relay_response(cdv_rdv *r, const rdv_action *a, uint8_t *out, size_t cap);
size_t rdv_request_relay(const cdv_rdv *r, const rdv_action *a, uint8_t *out, size_t cap);
size_t rdv_local_addr(cdv_rdv *r, const rdv_action *a, uint32_t my_ip, uint16_t my_port,
                      uint8_t *out, size_t cap);

/* LAN discovery: the pong for a ping datagram, or 0 if it was not one (or
 * was our own). `advertise` is the ID if registered, else our address. */
size_t lan_answer(const uint8_t *d, size_t n, const char *my_id, const char *advertise,
                  const char *hostname, uint8_t *out, size_t cap);

/* A fresh random uuid (v4, text form), for PunchHole. */
void rdv_uuid4(char out[37]);

/* Split "host[:port]". */
void rdv_split_host(const char *s, char *host, size_t cap, uint16_t *port, uint16_t def);

/* The socket-address obfuscation hbbs uses (rendezvous.rs mangle_*). */
size_t rdv_mangle(uint32_t ip, uint16_t port, uint32_t tm, uint8_t out[16]);
void rdv_unmangle(const uint8_t *b, size_t n, uint32_t *ip, uint16_t *port);

#endif
