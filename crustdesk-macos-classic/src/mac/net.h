/* TCP and UDP streams over MacTCP, driven by polling.
 *
 * No completion routines: whoever drives a stream -- the engine, at
 * deferred-task time -- looks at each parameter block's ioResult, which MacTCP
 * moves from `inProgress` (1) to the result when the call finishes. Every
 * call the engine makes here is asynchronous. Creating and releasing streams
 * are synchronous and belong to the main loop.
 *
 * A TCP stream listens or connects, carries one connection, and is reset by
 * an asynchronous abort that leaves it ready to listen or connect again --
 * so a peer can reconnect while the application is stuck behind a menu.
 */
#ifndef CDV_NET_H
#define CDV_NET_H

#include "mactcp.h"

#include <stddef.h>
#include <stdint.h>

/* Main loop: open the .IPP driver, learn our address. */
OSErr net_open(ip_addr *local, long *netmask);
void net_addr_string(ip_addr a, char *out);

enum { T_IDLE, T_LISTEN, T_CONNECT, T_OPEN, T_CLOSING, T_ABORTING };

typedef struct {
    StreamPtr stream;
    Ptr streambuf;
    long bufsize;
    volatile int used;      /* has been opened since it was created */
    volatile int renew_req; /* the engine wants a fresh stream (main loop) */
    uint8_t *rx;
    size_t rxcap;
    int state;
    TCPiopb open_pb, rcv_pb, snd_pb, ctl_pb;
    int rcv_busy, snd_busy;
    size_t snd_len;
    OSErr err;
    ip_addr remote;
    tcp_port remote_port, local_port;
    wdsEntry wds[2];
} cdv_tcp;

OSErr tcp_create(cdv_tcp *t, long streambuf, size_t rxcap); /* main loop */
void tcp_release(cdv_tcp *t);                                /* main loop */
/* A fresh stream on the same buffers. Open Transport's MacTCP cannot make a
 * second outgoing connection on a stream once its first has closed
 * (openFailed, -23015, at once), so an active open gets a new stream; the
 * engine asks with renew_req and the main loop does it. */
OSErr tcp_renew(cdv_tcp *t);                                 /* main loop */

void tcp_listen(cdv_tcp *t, tcp_port port);
void tcp_connect(cdv_tcp *t, ip_addr ip, tcp_port port);
/* Advance a stream. Returns 1 when a listen or connect has just produced a
 * connection, -1 when one has just failed, 0 otherwise; a close or abort
 * that finishes brings the stream back to T_IDLE. */
int tcp_poll(cdv_tcp *t);
int tcp_recv(cdv_tcp *t, const uint8_t **data, size_t *len);
void tcp_send(cdv_tcp *t, const uint8_t *data, size_t len); /* bytes stay put until sent */
size_t tcp_sent(cdv_tcp *t);                                /* what the last send delivered */
int tcp_send_idle(const cdv_tcp *t);
void tcp_close(cdv_tcp *t); /* graceful, then reset */
void tcp_abort(cdv_tcp *t); /* reset now */
int tcp_failed(const cdv_tcp *t);

typedef struct {
    StreamPtr stream;
    Ptr streambuf;
    uint16_t port;
    UDPiopb rcv_pb, snd_pb, ret_pb;
    int rcv_busy, have, snd_busy;
    uint8_t sbuf[1024];
    wdsEntry wds[2];
} cdv_udp;

OSErr udp_create(cdv_udp *u, uint16_t port, long streambuf); /* main loop; 0: any port */
void udp_release(cdv_udp *u);                                /* main loop */
/* A datagram, if one has arrived: valid until udp_done. */
int udp_recv(cdv_udp *u, const uint8_t **data, size_t *len, ip_addr *from, uint16_t *port);
void udp_done(cdv_udp *u);
/* Send (copied) if the last send has finished; returns nonzero if it went. */
int udp_send(cdv_udp *u, ip_addr to, uint16_t port, const uint8_t *data, size_t len);

#endif
