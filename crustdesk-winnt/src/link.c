/* The agent's end of the link to the tray program (link.h): a thread that
 * serves the named pipe, one tray at a time, with a queue each way so the
 * main loop never waits on it. */
#include "link.h"
#include "log.h"
#include "plat.h"

#include "win.h"
#include <stdlib.h>
#include <string.h>

/* ---- NT's pipe and security calls, looked up at run time ------------------
 *
 * The x86 build loads on Windows 95 and 98 too, and those refuse a program
 * that imports a function their DLLs do not export (check-imports.py). */

typedef HANDLE(WINAPI *CreateNamedPipeA_t)(LPCSTR, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                           LPSECURITY_ATTRIBUTES);
typedef BOOL(WINAPI *ConnectNamedPipe_t)(HANDLE, LPOVERLAPPED);
typedef BOOL(WINAPI *DisconnectNamedPipe_t)(HANDLE);
typedef BOOL(WINAPI *ImpersonateNamedPipeClient_t)(HANDLE);
typedef BOOL(WINAPI *RevertToSelf_t)(void);
typedef BOOL(WINAPI *OpenThreadToken_t)(HANDLE, DWORD, BOOL, PHANDLE);
typedef BOOL(WINAPI *InitializeSecurityDescriptor_t)(PSECURITY_DESCRIPTOR, DWORD);
typedef BOOL(WINAPI *SetSecurityDescriptorDacl_t)(PSECURITY_DESCRIPTOR, BOOL, PACL, BOOL);
typedef BOOL(WINAPI *InitializeAcl_t)(PACL, DWORD, DWORD);
typedef BOOL(WINAPI *AddAccessAllowedAce_t)(PACL, DWORD, DWORD, PSID);
typedef BOOL(WINAPI *AllocateAndInitializeSid_t)(PSID_IDENTIFIER_AUTHORITY, BYTE, DWORD, DWORD,
                                                 DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, PSID *);
typedef PVOID(WINAPI *FreeSid_t)(PSID);
typedef BOOL(WINAPI *EqualSid_t)(PSID, PSID);
typedef DWORD(WINAPI *GetLengthSid_t)(PSID);
typedef BOOL(WINAPI *CancelIo_t)(HANDLE);

static struct {
    CreateNamedPipeA_t CreateNamedPipeA;
    ConnectNamedPipe_t ConnectNamedPipe;
    DisconnectNamedPipe_t DisconnectNamedPipe;
    ImpersonateNamedPipeClient_t ImpersonateNamedPipeClient;
    RevertToSelf_t RevertToSelf;
    OpenThreadToken_t OpenThreadToken;
    InitializeSecurityDescriptor_t InitializeSecurityDescriptor;
    SetSecurityDescriptorDacl_t SetSecurityDescriptorDacl;
    InitializeAcl_t InitializeAcl;
    AddAccessAllowedAce_t AddAccessAllowedAce;
    AllocateAndInitializeSid_t AllocateAndInitializeSid;
    FreeSid_t FreeSid;
    EqualSid_t EqualSid;
    GetLengthSid_t GetLengthSid;
    CancelIo_t CancelIo;
} api;

static int load_api(void)
{
    HMODULE k = GetModuleHandle("kernel32.dll"), a = LoadLibrary("advapi32.dll");
    if (!k || !a)
        return 0;
    api.CreateNamedPipeA = (CreateNamedPipeA_t)GetProcAddress(k, "CreateNamedPipeA");
    api.ConnectNamedPipe = (ConnectNamedPipe_t)GetProcAddress(k, "ConnectNamedPipe");
    api.DisconnectNamedPipe = (DisconnectNamedPipe_t)GetProcAddress(k, "DisconnectNamedPipe");
    api.ImpersonateNamedPipeClient =
        (ImpersonateNamedPipeClient_t)GetProcAddress(a, "ImpersonateNamedPipeClient");
    api.RevertToSelf = (RevertToSelf_t)GetProcAddress(a, "RevertToSelf");
    api.OpenThreadToken = (OpenThreadToken_t)GetProcAddress(a, "OpenThreadToken");
    api.InitializeSecurityDescriptor =
        (InitializeSecurityDescriptor_t)GetProcAddress(a, "InitializeSecurityDescriptor");
    api.SetSecurityDescriptorDacl =
        (SetSecurityDescriptorDacl_t)GetProcAddress(a, "SetSecurityDescriptorDacl");
    api.InitializeAcl = (InitializeAcl_t)GetProcAddress(a, "InitializeAcl");
    api.AddAccessAllowedAce = (AddAccessAllowedAce_t)GetProcAddress(a, "AddAccessAllowedAce");
    api.AllocateAndInitializeSid =
        (AllocateAndInitializeSid_t)GetProcAddress(a, "AllocateAndInitializeSid");
    api.FreeSid = (FreeSid_t)GetProcAddress(a, "FreeSid");
    api.EqualSid = (EqualSid_t)GetProcAddress(a, "EqualSid");
    api.GetLengthSid = (GetLengthSid_t)GetProcAddress(a, "GetLengthSid");
    api.CancelIo = (CancelIo_t)GetProcAddress(k, "CancelIo");
    return api.CreateNamedPipeA && api.ConnectNamedPipe && api.DisconnectNamedPipe &&
           api.ImpersonateNamedPipeClient && api.RevertToSelf && api.OpenThreadToken &&
           api.InitializeSecurityDescriptor && api.SetSecurityDescriptorDacl &&
           api.InitializeAcl && api.AddAccessAllowedAce && api.AllocateAndInitializeSid &&
           api.FreeSid && api.EqualSid && api.GetLengthSid && api.CancelIo;
}

/* ---- the queues --------------------------------------------------------------- */

typedef struct msg {
    struct msg *next;
    int type;
    size_t n;
    char data[1];
} msg;

typedef struct {
    msg *head, *tail;
    int count;
} queue;

#define QUEUE_MAX 64 /* a tray that stops reading loses messages, not memory */

static CRITICAL_SECTION lock;
static queue to_tray, from_tray;
static HANDLE out_ev, stop_ev, thread;
static volatile LONG connected;
static BYTE console_sid[256];
static size_t console_sid_len;

static void q_put(queue *q, int type, const char *data, size_t n)
{
    msg *m;
    if (q->count >= QUEUE_MAX)
        return;
    m = (msg *)malloc(sizeof *m + n);
    if (!m)
        return;
    m->next = NULL;
    m->type = type;
    m->n = n;
    if (n)
        memcpy(m->data, data, n);
    m->data[n] = 0;
    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
    q->count++;
}

static msg *q_get(queue *q)
{
    msg *m = q->head;
    if (!m)
        return NULL;
    q->head = m->next;
    if (!q->head)
        q->tail = NULL;
    q->count--;
    return m;
}

static void q_clear(queue *q)
{
    msg *m;
    while ((m = q_get(q)) != NULL)
        free(m);
}

/* ---- the pipe ------------------------------------------------------------------- */

/* Anyone may open the pipe to read and write it; nobody but SYSTEM (the
 * agent) may make another instance of it, which is what would let another
 * program pose as the agent while it runs. */
static SECURITY_ATTRIBUTES *pipe_security(void)
{
    static SECURITY_DESCRIPTOR sd;
    static SECURITY_ATTRIBUTES sa;
    static DWORD aclbuf[64];
    SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY, nt = SECURITY_NT_AUTHORITY;
    PSID everyone = NULL, system = NULL;
    PACL acl = (PACL)aclbuf;
    int ok;
    if (!api.AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0,
                                      &everyone))
        return NULL;
    if (!api.AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
                                      &system)) {
        api.FreeSid(everyone);
        return NULL;
    }
    ok = api.InitializeAcl(acl, sizeof aclbuf, ACL_REVISION) &&
         api.AddAccessAllowedAce(acl, ACL_REVISION,
                                 FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                                 everyone) &&
         api.AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, system) &&
         api.InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
         api.SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    api.FreeSid(everyone);
    api.FreeSid(system);
    if (!ok)
        return NULL;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}

/* Wait for an overlapped operation, or the stop event, or `ms`. Bytes moved,
 * or -1. */
static long finish(HANDLE p, OVERLAPPED *ov, DWORD ms)
{
    HANDLE w[2];
    DWORD got = 0, r;
    w[0] = ov->hEvent;
    w[1] = stop_ev;
    r = WaitForMultipleObjects(2, w, FALSE, ms);
    if (r != WAIT_OBJECT_0) {
        api.CancelIo(p);
        return -1;
    }
    if (!GetOverlappedResult(p, ov, &got, FALSE))
        return -1;
    return (long)got;
}

static int start_read(HANDLE p, char *buf, OVERLAPPED *ov)
{
    DWORD got;
    ResetEvent(ov->hEvent);
    if (!ReadFile(p, buf, LINK_MAX, &got, ov) && GetLastError() != ERROR_IO_PENDING)
        return 0;
    return 1;
}

static int write_msg(HANDLE p, int type, const char *data, size_t n)
{
    static char buf[LINK_MAX];
    OVERLAPPED ov;
    DWORD put;
    long r;
    if (n > LINK_MAX - 1)
        n = LINK_MAX - 1;
    buf[0] = (char)type;
    memcpy(buf + 1, data, n);
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent)
        return 0;
    if (!WriteFile(p, buf, (DWORD)n + 1, &put, &ov) && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return 0;
    }
    r = finish(p, &ov, 10000);
    CloseHandle(ov.hEvent);
    return r == (long)n + 1;
}

/* The tray's hello, and is it the console user? */
static int accept_tray(HANDLE p, char *buf, OVERLAPPED *rov)
{
    HANDLE tok;
    DWORD got;
    BYTE info[512];
    int ok = 0;
    long n;
    if (!start_read(p, buf, rov) || (n = finish(p, rov, 5000)) < 1 || buf[0] != LINK_HELLO)
        return 0;
    if (!api.ImpersonateNamedPipeClient(p)) {
        cdv_log("tray: cannot tell who it is (error %lu)", GetLastError());
        return 0;
    }
    if (api.OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok)) {
        if (GetTokenInformation(tok, TokenUser, info, sizeof info, &got)) {
            PSID sid = ((TOKEN_USER *)info)->User.Sid;
            EnterCriticalSection(&lock);
            ok = console_sid_len && api.EqualSid(sid, (PSID)console_sid);
            LeaveCriticalSection(&lock);
        }
        CloseHandle(tok);
    }
    api.RevertToSelf();
    if (!ok) {
        static const char no[] = "Only the user logged on at this machine's console may use "
                                 "the C-Desk-Vint tray.";
        cdv_log("tray: refused a program that is not the console user's");
        write_msg(p, LINK_REFUSED, no, sizeof no - 1);
        Sleep(500); /* hanging up throws away what the tray has not read yet */
    }
    return ok;
}

/* One tray, until it goes or the agent stops. */
static void serve(HANDLE p)
{
    static char buf[LINK_MAX + 1];
    OVERLAPPED rov;
    HANDLE w[3];
    int alive = 1;
    memset(&rov, 0, sizeof rov);
    rov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!rov.hEvent)
        return;
    if (!accept_tray(p, buf, &rov)) {
        CloseHandle(rov.hEvent);
        return;
    }
    EnterCriticalSection(&lock);
    q_clear(&to_tray);
    q_put(&from_tray, LINK_JOINED, NULL, 0);
    LeaveCriticalSection(&lock);
    InterlockedExchange((LONG *)&connected, 1);
    cdv_log("tray: connected");
    if (!start_read(p, buf, &rov))
        alive = 0;
    w[0] = rov.hEvent;
    w[1] = out_ev;
    w[2] = stop_ev;
    while (alive) {
        DWORD r = WaitForMultipleObjects(3, w, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            DWORD got = 0;
            if (!GetOverlappedResult(p, &rov, &got, FALSE) || got < 1)
                break;
            EnterCriticalSection(&lock);
            q_put(&from_tray, (unsigned char)buf[0], buf + 1, got - 1);
            LeaveCriticalSection(&lock);
            if (!start_read(p, buf, &rov))
                break;
        } else if (r == WAIT_OBJECT_0 + 1) {
            msg *m;
            ResetEvent(out_ev);
            for (;;) {
                EnterCriticalSection(&lock);
                m = q_get(&to_tray);
                LeaveCriticalSection(&lock);
                if (!m)
                    break;
                if (!write_msg(p, m->type, m->data, m->n))
                    alive = 0;
                free(m);
                if (!alive)
                    break;
            }
        } else {
            break;
        }
    }
    api.CancelIo(p);
    CloseHandle(rov.hEvent);
    InterlockedExchange((LONG *)&connected, 0);
    cdv_log("tray: gone");
}

static DWORD WINAPI pipe_thread(LPVOID unused)
{
    SECURITY_ATTRIBUTES *sa = pipe_security();
    int warned = 0;
    (void)unused;
    if (!sa)
        cdv_log("tray: cannot secure the pipe (error %lu); no tray", GetLastError());
    while (sa && WaitForSingleObject(stop_ev, 0) != WAIT_OBJECT_0) {
        OVERLAPPED ov;
        HANDLE p = api.CreateNamedPipeA(LINK_PIPE, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
                                        LINK_MAX, LINK_MAX, 0, sa);
        if (p == INVALID_HANDLE_VALUE) {
            if (!warned)
                cdv_log("tray: cannot make the pipe (error %lu; is another agent running?)",
                        GetLastError());
            warned = 1;
            WaitForSingleObject(stop_ev, 5000);
            continue;
        }
        warned = 0;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (ov.hEvent) {
            int ready = api.ConnectNamedPipe(p, &ov);
            DWORD e = GetLastError();
            if (!ready && e == ERROR_PIPE_CONNECTED)
                ready = 1;
            else if (!ready && e == ERROR_IO_PENDING)
                ready = finish(p, &ov, INFINITE) >= 0;
            CloseHandle(ov.hEvent);
            if (ready)
                serve(p);
        }
        api.DisconnectNamedPipe(p);
        CloseHandle(p);
    }
    return 0;
}

/* ---- the agent's side ----------------------------------------------------------- */

int link_start(void)
{
    if (!plat_nt() || !load_api())
        return 0;
    InitializeCriticalSection(&lock);
    out_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    stop_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!out_ev || !stop_ev)
        return 0;
    thread = plat_thread(pipe_thread, NULL);
    return thread != NULL;
}

void link_stop(void)
{
    if (!thread)
        return;
    SetEvent(stop_ev);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    thread = NULL;
}

int link_connected(void)
{
    return thread && connected;
}

void link_send(int type, const char *data, size_t n)
{
    if (!link_connected())
        return;
    EnterCriticalSection(&lock);
    q_put(&to_tray, type, data, n);
    LeaveCriticalSection(&lock);
    SetEvent(out_ev);
}

long link_take(int *type, char *out, size_t cap)
{
    msg *m;
    size_t n;
    if (!thread)
        return -1;
    EnterCriticalSection(&lock);
    m = q_get(&from_tray);
    LeaveCriticalSection(&lock);
    if (!m)
        return -1;
    n = m->n < cap - 1 ? m->n : cap - 1;
    memcpy(out, m->data, n);
    out[n] = 0;
    *type = m->type;
    free(m);
    return (long)n;
}

void link_set_console_sid(const void *sid)
{
    size_t n;
    if (!thread)
        return;
    n = sid ? (size_t)api.GetLengthSid((PSID)sid) : 0;
    if (n > sizeof console_sid)
        n = 0;
    EnterCriticalSection(&lock);
    if (n)
        memcpy(console_sid, sid, n);
    console_sid_len = n;
    LeaveCriticalSection(&lock);
}
