#include "net.h"

#include <stdio.h>
#include <string.h>

#define ULP_TIMEOUT 60   /* seconds of unacknowledged data before giving up */
#define SEND_CHUNK 16384 /* per TCPSend; one wdsEntry holds < 64 KB */
#define CONNECT_TIMEOUT 10

static short ipp; /* the .IPP driver */

OSErr net_open(ip_addr *local, long *netmask)
{
    IPGetAddrPB ga;
    OSErr err = OpenDriver("\p.IPP", &ipp);
    if (err != noErr)
        return err;
    memset(&ga, 0, sizeof ga);
    ga.ioCRefNum = ipp;
    ga.csCode = ipctlGetAddr;
    if (PBControlSync((ParmBlkPtr)&ga) == noErr) {
        *local = ga.ourAddress;
        *netmask = ga.ourNetMask;
    }
    return noErr;
}

void net_addr_string(ip_addr a, char *out)
{
    sprintf(out, "%lu.%lu.%lu.%lu", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

/* ---- TCP ------------------------------------------------------------------ */

static void pb_prep(cdv_tcp *t, TCPiopb *pb, short cs)
{
    memset(pb, 0, sizeof *pb);
    pb->ioCRefNum = ipp;
    pb->csCode = cs;
    pb->tcpStream = t->stream;
}

static void go(TCPiopb *pb)
{
    pb->ioResult = inProgress;
    PBControlAsync((ParmBlkPtr)pb);
}

OSErr tcp_create(cdv_tcp *t, long bufsize, size_t rxcap)
{
    TCPiopb pb;
    OSErr err;
    memset(t, 0, sizeof *t);
    t->streambuf = NewPtr(bufsize);
    t->rx = (uint8_t *)NewPtr((long)rxcap);
    if (!t->streambuf || !t->rx)
        return memFullErr;
    t->rxcap = rxcap;
    t->bufsize = bufsize;
    pb_prep(t, &pb, TCPCreate);
    pb.csParam.create.rcvBuff = t->streambuf;
    pb.csParam.create.rcvBuffLen = (unsigned long)bufsize;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr)
        t->stream = pb.tcpStream;
    t->state = T_IDLE;
    return err;
}

void tcp_release(cdv_tcp *t)
{
    TCPiopb pb;
    if (!t->stream)
        return;
    pb_prep(t, &pb, TCPAbort);
    PBControlSync((ParmBlkPtr)&pb);
    pb_prep(t, &pb, TCPRelease);
    PBControlSync((ParmBlkPtr)&pb);
    t->stream = 0;
}

OSErr tcp_renew(cdv_tcp *t)
{
    TCPiopb pb;
    OSErr err;
    tcp_release(t);
    pb_prep(t, &pb, TCPCreate);
    pb.csParam.create.rcvBuff = t->streambuf;
    pb.csParam.create.rcvBuffLen = (unsigned long)t->bufsize;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr)
        t->stream = pb.tcpStream;
    t->state = T_IDLE;
    t->rcv_busy = t->snd_busy = 0;
    t->used = 0;
    t->renew_req = 0;
    return err;
}

static void open_common(cdv_tcp *t, int active, ip_addr ip, tcp_port port)
{
    TCPiopb *pb = &t->open_pb;
    pb_prep(t, pb, active ? TCPActiveOpen : TCPPassiveOpen);
    pb->csParam.open.ulpTimeoutValue = ULP_TIMEOUT;
    pb->csParam.open.ulpTimeoutAction = 1; /* abort */
    pb->csParam.open.validityFlags = (SInt8)0xC0;
    pb->csParam.open.commandTimeoutValue = active ? CONNECT_TIMEOUT : 0; /* 0: for ever */
    if (active) {
        /* A fresh local port each time. Left at 0, Open Transport's MacTCP
         * gives the stream the port of its last connection, which is still
         * in TIME_WAIT: openFailed (-23015) at once, every time. */
        static tcp_port next;
        if (!next)
            next = (tcp_port)(49152 + TickCount() % 8192);
        if (++next < 49152)
            next = 49152;
        pb->csParam.open.remoteHost = ip;
        pb->csParam.open.remotePort = port;
        pb->csParam.open.localPort = next;
    } else {
        pb->csParam.open.localPort = port;
    }
    t->err = noErr;
    t->rcv_busy = t->snd_busy = 0;
    t->used = 1;
    t->state = active ? T_CONNECT : T_LISTEN;
    go(pb);
}

void tcp_listen(cdv_tcp *t, tcp_port port)
{
    t->local_port = port;
    open_common(t, 0, 0, port);
}

void tcp_connect(cdv_tcp *t, ip_addr ip, tcp_port port)
{
    open_common(t, 1, ip, port);
}

static int all_back(const cdv_tcp *t)
{
    return t->open_pb.ioResult != inProgress && t->ctl_pb.ioResult != inProgress &&
           !(t->rcv_busy && t->rcv_pb.ioResult == inProgress) &&
           !(t->snd_busy && t->snd_pb.ioResult == inProgress);
}

int tcp_poll(cdv_tcp *t)
{
    short r;
    switch (t->state) {
    case T_LISTEN:
    case T_CONNECT:
        r = t->open_pb.ioResult;
        if (r == inProgress)
            return 0;
        if (r != noErr) {
            t->err = r;
            tcp_abort(t);
            return -1;
        }
        t->remote = t->open_pb.csParam.open.remoteHost;
        t->remote_port = t->open_pb.csParam.open.remotePort;
        t->local_port = t->open_pb.csParam.open.localPort;
        t->state = T_OPEN;
        return 1;
    case T_CLOSING:
        if (t->ctl_pb.ioResult == inProgress)
            return 0;
        tcp_abort(t); /* the FIN is out; now reset the stream for reuse */
        return 0;
    case T_ABORTING:
        if (all_back(t)) {
            t->state = T_IDLE;
            t->rcv_busy = t->snd_busy = 0;
        }
        return 0;
    default:
        return 0;
    }
}

int tcp_recv(cdv_tcp *t, const uint8_t **data, size_t *len)
{
    TCPiopb *pb = &t->rcv_pb;
    short r;
    if (t->state != T_OPEN || t->err)
        return 0;
    if (!t->rcv_busy) {
        pb_prep(t, pb, TCPRcv);
        pb->csParam.receive.commandTimeoutValue = 0;
        pb->csParam.receive.rcvBuff = (Ptr)t->rx;
        pb->csParam.receive.rcvBuffLen = (unsigned short)t->rxcap;
        t->rcv_busy = 1;
        go(pb);
        return 0;
    }
    r = pb->ioResult;
    if (r == inProgress)
        return 0;
    t->rcv_busy = 0;
    if (r == commandTimeout)
        return 0;
    if (r != noErr) {
        t->err = r;
        return 0;
    }
    *data = t->rx;
    *len = pb->csParam.receive.rcvBuffLen;
    return *len != 0;
}

void tcp_send(cdv_tcp *t, const uint8_t *data, size_t len)
{
    TCPiopb *pb = &t->snd_pb;
    if (t->state != T_OPEN || t->snd_busy || t->err || !len)
        return;
    if (len > SEND_CHUNK)
        len = SEND_CHUNK;
    t->wds[0].length = (unsigned short)len;
    t->wds[0].ptr = (Ptr)data;
    t->wds[1].length = 0;
    t->wds[1].ptr = NULL;
    pb_prep(t, pb, TCPSend);
    pb->csParam.send.ulpTimeoutValue = ULP_TIMEOUT;
    pb->csParam.send.ulpTimeoutAction = 1;
    pb->csParam.send.validityFlags = (SInt8)0xC0;
    pb->csParam.send.pushFlag = 1;
    pb->csParam.send.wdsPtr = (Ptr)t->wds;
    t->snd_busy = 1;
    t->snd_len = len;
    go(pb);
}

size_t tcp_sent(cdv_tcp *t)
{
    short r;
    if (!t->snd_busy)
        return 0;
    r = t->snd_pb.ioResult;
    if (r == inProgress)
        return 0;
    t->snd_busy = 0;
    if (r != noErr) {
        t->err = r;
        return 0;
    }
    return t->snd_len;
}

int tcp_send_idle(const cdv_tcp *t)
{
    return !t->snd_busy;
}

int tcp_failed(const cdv_tcp *t)
{
    return t->state == T_OPEN && t->err != noErr;
}

void tcp_close(cdv_tcp *t)
{
    TCPiopb *pb = &t->ctl_pb;
    if (t->state != T_OPEN) {
        tcp_abort(t);
        return;
    }
    pb_prep(t, pb, TCPClose);
    pb->csParam.close.ulpTimeoutValue = 10;
    pb->csParam.close.ulpTimeoutAction = 1;
    pb->csParam.close.validityFlags = (SInt8)0xC0;
    t->state = T_CLOSING;
    go(pb);
}

void tcp_abort(cdv_tcp *t)
{
    TCPiopb *pb = &t->ctl_pb;
    if (t->state == T_ABORTING || !t->stream)
        return;
    if (pb->ioResult == inProgress && t->state == T_CLOSING) {
        /* a close is still out; abort anyway -- it completes the close too */
    }
    pb_prep(t, pb, TCPAbort);
    t->state = T_ABORTING;
    go(pb);
}

/* ---- UDP ------------------------------------------------------------------ */

static void upb_prep(cdv_udp *u, UDPiopb *pb, short cs)
{
    memset(pb, 0, sizeof *pb);
    pb->ioCRefNum = ipp;
    pb->csCode = cs;
    pb->udpStream = u->stream;
}

OSErr udp_create(cdv_udp *u, uint16_t port, long bufsize)
{
    UDPiopb pb;
    OSErr err;
    memset(u, 0, sizeof *u);
    u->streambuf = NewPtr(bufsize);
    if (!u->streambuf)
        return memFullErr;
    upb_prep(u, &pb, UDPCreate);
    pb.csParam.create.rcvBuff = u->streambuf;
    pb.csParam.create.rcvBuffLen = (unsigned long)bufsize;
    pb.csParam.create.localPort = port;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr) {
        u->stream = pb.udpStream;
        u->port = pb.csParam.create.localPort;
    }
    return err;
}

void udp_release(cdv_udp *u)
{
    UDPiopb pb;
    if (!u->stream)
        return;
    upb_prep(u, &pb, UDPRelease);
    PBControlSync((ParmBlkPtr)&pb);
    u->stream = 0;
}

int udp_recv(cdv_udp *u, const uint8_t **data, size_t *len, ip_addr *from, uint16_t *port)
{
    UDPiopb *pb = &u->rcv_pb;
    short r;
    if (!u->stream || u->have)
        return 0;
    if (u->ret_pb.ioResult == inProgress)
        return 0; /* the last buffer is still going back */
    if (!u->rcv_busy) {
        upb_prep(u, pb, UDPRead);
        pb->csParam.receive.timeOut = 0;
        u->rcv_busy = 1;
        pb->ioResult = inProgress;
        PBControlAsync((ParmBlkPtr)pb);
        return 0;
    }
    r = pb->ioResult;
    if (r == inProgress)
        return 0;
    u->rcv_busy = 0;
    if (r != noErr)
        return 0;
    *data = (const uint8_t *)pb->csParam.receive.rcvBuff;
    *len = pb->csParam.receive.rcvBuffLen;
    *from = pb->csParam.receive.remoteHost;
    *port = pb->csParam.receive.remotePort;
    u->have = 1;
    return 1;
}

void udp_done(cdv_udp *u)
{
    UDPiopb *pb = &u->ret_pb;
    if (!u->have)
        return;
    upb_prep(u, pb, UDPBfrReturn);
    pb->csParam.receive.rcvBuff = u->rcv_pb.csParam.receive.rcvBuff;
    pb->ioResult = inProgress;
    PBControlAsync((ParmBlkPtr)pb);
    u->have = 0;
}

int udp_send(cdv_udp *u, ip_addr to, uint16_t port, const uint8_t *data, size_t len)
{
    UDPiopb *pb = &u->snd_pb;
    if (!u->stream || len > sizeof u->sbuf)
        return 0;
    if (u->snd_busy && pb->ioResult == inProgress)
        return 0;
    memcpy(u->sbuf, data, len);
    u->wds[0].length = (unsigned short)len;
    u->wds[0].ptr = (Ptr)u->sbuf;
    u->wds[1].length = 0;
    u->wds[1].ptr = NULL;
    upb_prep(u, pb, UDPWrite);
    pb->csParam.send.remoteHost = to;
    pb->csParam.send.remotePort = port;
    pb->csParam.send.wdsPtr = (Ptr)u->wds;
    pb->csParam.send.checkSum = 1;
    u->snd_busy = 1;
    pb->ioResult = inProgress;
    PBControlAsync((ParmBlkPtr)pb);
    return 1;
}
