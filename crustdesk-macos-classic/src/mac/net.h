/* One listening TCP stream over MacTCP, driven by polling.
 *
 * No completion routines: whoever drives it -- the engine, at deferred-task
 * time -- looks at each parameter block's ioResult, which MacTCP moves from
 * `inProgress` (1) to the result when the call finishes. Everything the engine
 * calls here is asynchronous, including dropping a connection and listening
 * again, so a peer can reconnect while the application is stuck behind a
 * menu. net_init, net_reset and net_shutdown make synchronous calls and
 * belong to the main loop.
 */
#ifndef CDV_NET_H
#define CDV_NET_H

#include "mactcp.h"

#include <stddef.h>
#include <stdint.h>

enum { NET_DOWN, NET_LISTENING, NET_CONNECTED, NET_RESETTING };

typedef struct {
    short refnum;
    StreamPtr stream;
    Ptr streambuf;
    int state;
    unsigned short port;
    ip_addr local, remote;
    OSErr err;

    TCPiopb open_pb, rcv_pb, snd_pb, abort_pb;
    int rcv_busy, snd_busy;
    size_t snd_len;
    wdsEntry wds[2];
    uint8_t *rxbuf;
    size_t rxcap;
} cdv_net;

/* Open the driver, make the stream, start listening. */
OSErr net_init(cdv_net *n, unsigned short port);

/* Nonzero once, when a peer has connected. */
int net_accepted(cdv_net *n);

/* Bytes received since the last call, if any. Valid until the next call. */
int net_recv(cdv_net *n, const uint8_t **data, size_t *len);

/* Start sending up to `len` bytes of `data` if no send is in flight. The
 * bytes must stay put until net_sent reports them. */
void net_send(cdv_net *n, const uint8_t *data, size_t len);

/* How many bytes the last send delivered, once it has finished; else 0. */
size_t net_sent(cdv_net *n);

int net_send_idle(const cdv_net *n);

/* Drop the connection and listen again: synchronously (main loop), or as an
 * asynchronous abort that net_accepted finishes (engine). */
void net_reset(cdv_net *n);
void net_reset_async(cdv_net *n);

/* Abort and release the stream. Before quitting, always. */
void net_shutdown(cdv_net *n);

/* Nonzero if the connection or the listen failed (the caller should reset). */
int net_failed(const cdv_net *n);

void net_addr_string(ip_addr a, char *out);

#endif
