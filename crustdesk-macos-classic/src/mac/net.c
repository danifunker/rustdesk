#include "net.h"

#include <stdio.h>
#include <string.h>

#define STREAM_BUFFER (32 * 1024L) /* MacTCP's own buffer for the stream */
#define RX_BUFFER (8 * 1024)
#define SEND_CHUNK 16384           /* per TCPSend; one wdsEntry holds < 64 KB */
#define ULP_TIMEOUT 60             /* seconds of unacknowledged data before giving up */

static void pb_prep(cdv_net *n, TCPiopb *pb, short cs)
{
    memset(pb, 0, sizeof *pb);
    pb->ioCRefNum = n->refnum;
    pb->csCode = cs;
    pb->tcpStream = n->stream;
}

static void listen_async(cdv_net *n)
{
    TCPiopb *pb = &n->open_pb;
    pb_prep(n, pb, TCPPassiveOpen);
    pb->csParam.open.ulpTimeoutValue = ULP_TIMEOUT;
    pb->csParam.open.ulpTimeoutAction = 1; /* abort */
    pb->csParam.open.validityFlags = (SInt8)0xC0;
    pb->csParam.open.commandTimeoutValue = 0; /* wait for ever */
    pb->csParam.open.localPort = n->port;
    pb->ioResult = inProgress;
    PBControlAsync((ParmBlkPtr)pb);
    n->state = NET_LISTENING;
    n->rcv_busy = n->snd_busy = 0;
    n->err = noErr;
}

void net_addr_string(ip_addr a, char *out)
{
    sprintf(out, "%lu.%lu.%lu.%lu", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

OSErr net_init(cdv_net *n, unsigned short port)
{
    IPGetAddrPB ga;
    TCPiopb pb;
    OSErr err;

    memset(n, 0, sizeof *n);
    n->port = port;
    err = OpenDriver("\p.IPP", &n->refnum);
    if (err != noErr)
        return err;

    memset(&ga, 0, sizeof ga);
    ga.ioCRefNum = n->refnum;
    ga.csCode = ipctlGetAddr;
    if (PBControlSync((ParmBlkPtr)&ga) == noErr)
        n->local = ga.ourAddress;

    n->streambuf = NewPtr(STREAM_BUFFER);
    n->rxbuf = (uint8_t *)NewPtr(RX_BUFFER);
    if (!n->streambuf || !n->rxbuf)
        return memFullErr;
    n->rxcap = RX_BUFFER;

    pb_prep(n, &pb, TCPCreate);
    pb.csParam.create.rcvBuff = n->streambuf;
    pb.csParam.create.rcvBuffLen = STREAM_BUFFER;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr)
        return err;
    n->stream = pb.tcpStream;
    listen_async(n);
    return noErr;
}

int net_accepted(cdv_net *n)
{
    short r;
    if (n->state != NET_LISTENING)
        return 0;
    r = n->open_pb.ioResult;
    if (r == inProgress)
        return 0;
    if (r != noErr) {
        /* A listen that failed (or timed out) just listens again. */
        TCPiopb pb;
        pb_prep(n, &pb, TCPAbort);
        PBControlSync((ParmBlkPtr)&pb);
        listen_async(n);
        return 0;
    }
    n->remote = n->open_pb.csParam.open.remoteHost;
    n->state = NET_CONNECTED;
    return 1;
}

static void recv_async(cdv_net *n)
{
    TCPiopb *pb = &n->rcv_pb;
    pb_prep(n, pb, TCPRcv);
    pb->csParam.receive.commandTimeoutValue = 0;
    pb->csParam.receive.rcvBuff = (Ptr)n->rxbuf;
    pb->csParam.receive.rcvBuffLen = (unsigned short)n->rxcap;
    pb->ioResult = inProgress;
    PBControlAsync((ParmBlkPtr)pb);
    n->rcv_busy = 1;
}

int net_recv(cdv_net *n, const uint8_t **data, size_t *len)
{
    short r;
    if (n->state != NET_CONNECTED || n->err)
        return 0;
    if (!n->rcv_busy) {
        recv_async(n);
        return 0;
    }
    r = n->rcv_pb.ioResult;
    if (r == inProgress)
        return 0;
    n->rcv_busy = 0;
    if (r == commandTimeout) /* nothing arrived; ask again */
        return 0;
    if (r != noErr) {
        n->err = r;
        return 0;
    }
    *data = n->rxbuf;
    *len = n->rcv_pb.csParam.receive.rcvBuffLen;
    return *len != 0;
}

void net_send(cdv_net *n, const uint8_t *data, size_t len)
{
    TCPiopb *pb = &n->snd_pb;
    if (n->state != NET_CONNECTED || n->snd_busy || n->err || !len)
        return;
    if (len > SEND_CHUNK)
        len = SEND_CHUNK;
    n->wds[0].length = (unsigned short)len;
    n->wds[0].ptr = (Ptr)data;
    n->wds[1].length = 0;
    n->wds[1].ptr = NULL;
    pb_prep(n, pb, TCPSend);
    pb->csParam.send.ulpTimeoutValue = ULP_TIMEOUT;
    pb->csParam.send.ulpTimeoutAction = 1;
    pb->csParam.send.validityFlags = (SInt8)0xC0;
    pb->csParam.send.pushFlag = 1;
    pb->csParam.send.wdsPtr = (Ptr)n->wds;
    pb->ioResult = inProgress;
    PBControlAsync((ParmBlkPtr)pb);
    n->snd_busy = 1;
    n->snd_len = len;
}

size_t net_sent(cdv_net *n)
{
    short r;
    if (!n->snd_busy)
        return 0;
    r = n->snd_pb.ioResult;
    if (r == inProgress)
        return 0;
    n->snd_busy = 0;
    if (r != noErr) {
        n->err = r;
        return 0;
    }
    return n->snd_len;
}

int net_send_idle(const cdv_net *n)
{
    return !n->snd_busy;
}

int net_failed(const cdv_net *n)
{
    return n->state == NET_CONNECTED && n->err != noErr;
}

void net_reset(cdv_net *n)
{
    TCPiopb pb;
    /* Abort completes any receive or send still in flight with an error,
     * so every parameter block is free again afterwards. */
    pb_prep(n, &pb, TCPAbort);
    PBControlSync((ParmBlkPtr)&pb);
    while (n->rcv_busy && n->rcv_pb.ioResult == inProgress)
        ;
    while (n->snd_busy && n->snd_pb.ioResult == inProgress)
        ;
    listen_async(n);
}

void net_shutdown(cdv_net *n)
{
    TCPiopb pb;
    if (!n->stream)
        return;
    pb_prep(n, &pb, TCPAbort);
    PBControlSync((ParmBlkPtr)&pb);
    pb_prep(n, &pb, TCPRelease);
    PBControlSync((ParmBlkPtr)&pb);
    n->stream = 0;
    n->state = NET_DOWN;
}
