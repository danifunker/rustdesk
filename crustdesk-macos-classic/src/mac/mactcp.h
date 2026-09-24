/* The part of MacTCP's interface C-Desk-Vint uses: TCP and UDP streams on
 * the .IPP driver, and the driver's own address.
 *
 * Retro68's Multiversal Interfaces have no MacTCP.h. These declarations are
 * the ABI -- the parameter blocks cross into MacTCP, so their layout is fixed
 * by it -- as documented in the MacTCP Programmer's Guide and Inside Macintosh:
 * Networking. Written for this project; not copied from Apple's header.
 *
 * Every call here is a PBControl on ".IPP", so the same code runs on MacTCP
 * itself and on Open Transport's MacTCP compatibility, 7.0 through 9.2.2.
 *
 * 68k alignment (2-byte) is what these layouts assume. That is m68k GCC's
 * default; PowerPC builds need it asked for.
 */
#ifndef CDV_MACTCP_H
#define CDV_MACTCP_H

#include <Multiverse.h>

#if defined(__powerpc__) || defined(__ppc__)
#pragma pack(push, 2)
#endif

typedef unsigned long ip_addr;
typedef unsigned short tcp_port;
typedef unsigned long StreamPtr;

typedef struct {
    unsigned short length; /* 0 ends the list */
    Ptr ptr;
} wdsEntry;

/* Driver control codes. */
enum {
    ipctlGetAddr = 15,
    UDPCreate = 20, UDPRead = 21, UDPBfrReturn = 22, UDPWrite = 23, UDPRelease = 24,
    TCPCreate = 30, TCPPassiveOpen = 31, TCPActiveOpen = 32, TCPSend = 34,
    TCPNoCopyRcv = 35, TCPRcvBfrReturn = 36, TCPRcv = 37, TCPClose = 38,
    TCPAbort = 39, TCPStatus = 40, TCPRelease = 42
};

/* Result codes worth naming. */
enum {
    inProgress = 1,
    connectionClosing = -23005,
    connectionExists = -23007,
    connectionTerminated = -23012,
    commandTimeout = -23016,
    duplicateSocket = -23017
};

typedef struct {
    Ptr rcvBuff;
    unsigned long rcvBuffLen;
    ProcPtr notifyProc;
    Ptr userDataPtr;
} TCPCreatePB;

typedef struct {
    SInt8 ulpTimeoutValue;
    SInt8 ulpTimeoutAction;
    SInt8 validityFlags;
    SInt8 commandTimeoutValue;
    ip_addr remoteHost;
    tcp_port remotePort;
    ip_addr localHost;
    tcp_port localPort;
    SInt8 tosFlags;
    SInt8 precedence;
    Boolean dontFrag;
    SInt8 timeToLive;
    SInt8 security;
    SInt8 optionCnt;
    SInt8 options[40];
    Ptr userDataPtr;
} TCPOpenPB;

typedef struct {
    SInt8 ulpTimeoutValue;
    SInt8 ulpTimeoutAction;
    SInt8 validityFlags;
    Boolean pushFlag;
    Boolean urgentFlag;
    SInt8 filler;
    Ptr wdsPtr;
    unsigned long sendFree;
    unsigned short sendLength;
    Ptr userDataPtr;
} TCPSendPB;

typedef struct {
    SInt8 commandTimeoutValue;
    Boolean markFlag;
    Boolean urgentFlag;
    SInt8 filler;
    Ptr rcvBuff;
    unsigned short rcvBuffLen;
    Ptr rdsPtr;
    unsigned short rdsLength;
    unsigned short secondTimeStamp;
    Ptr userDataPtr;
} TCPReceivePB;

typedef struct {
    SInt8 ulpTimeoutValue;
    SInt8 ulpTimeoutAction;
    SInt8 validityFlags;
    SInt8 filler;
    Ptr userDataPtr;
} TCPClosePB;

typedef struct {
    SInt8 fill12[12];
    ProcPtr ioCompletion;
    volatile short ioResult; /* written by MacTCP while we poll it */
    Ptr ioNamePtr;
    short ioVRefNum;
    short ioCRefNum;
    short csCode;
    StreamPtr tcpStream;
    union {
        TCPCreatePB create;
        TCPOpenPB open;
        TCPSendPB send;
        TCPReceivePB receive;
        TCPClosePB close;
        char pad[96];
    } csParam;
} TCPiopb;

typedef struct {
    SInt8 fill12[12];
    ProcPtr ioCompletion;
    volatile short ioResult;
    Ptr ioNamePtr;
    short ioVRefNum;
    short ioCRefNum;
    short csCode;
    ip_addr ourAddress;
    long ourNetMask;
} IPGetAddrPB;

typedef unsigned short udp_port;

typedef struct {
    Ptr rcvBuff;
    unsigned long rcvBuffLen;
    ProcPtr notifyProc;
    unsigned short localPort;
    Ptr userDataPtr;
    udp_port endingPort;
} UDPCreatePB;

typedef struct {
    unsigned short reserved;
    ip_addr remoteHost;
    udp_port remotePort;
    Ptr wdsPtr;
    Boolean checkSum;
    SInt8 filler;
    unsigned short sendLength;
    Ptr userDataPtr;
    udp_port localPort;
} UDPSendPB;

typedef struct {
    unsigned short timeOut; /* seconds */
    ip_addr remoteHost;
    udp_port remotePort;
    Ptr rcvBuff;
    unsigned short rcvBuffLen;
    unsigned short secondTimeStamp;
    Ptr userDataPtr;
    ip_addr destHost;
    udp_port destPort;
} UDPReceivePB;

typedef struct {
    SInt8 fill12[12];
    ProcPtr ioCompletion;
    volatile short ioResult;
    Ptr ioNamePtr;
    short ioVRefNum;
    short ioCRefNum;
    short csCode;
    StreamPtr udpStream;
    union {
        UDPCreatePB create;
        UDPSendPB send;
        UDPReceivePB receive;
        char pad[64];
    } csParam;
} UDPiopb;

#if defined(__powerpc__) || defined(__ppc__)
#pragma pack(pop)
#endif

#endif
