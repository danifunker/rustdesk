/* ntexec: a build helper for the Windows NT test machine.
 *
 * Listens on TCP 2370 and serves one request per connection:
 *
 *   RUN <cmdline>\n        runs `cmd /c <cmdline>`, sends its output, then
 *                          "\n@@EXIT <code>\n"
 *   SPAWN <cmdline>\n      starts a process detached; replies "OK <pid>\n"
 *   PUT <path> <size>\n    then <size> bytes; replies "OK\n"
 *   GET <path>\n           replies "<size>\n" then the bytes ("-1\n" if none)
 *
 * No authentication: it is for a VM whose port is reachable only through
 * QEMU's user-mode NAT forward on the host's loopback.  Never install it on
 * a machine anyone else can reach.
 *
 * Build on NT:  cl ntexec.c wsock32.lib
 */
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sendall(SOCKET s, const char *p, int n)
{
    while (n > 0) {
        int k = send(s, p, n, 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}

static int recvline(SOCKET s, char *buf, int max)
{
    int n = 0;
    while (n < max - 1) {
        char c;
        if (recv(s, &c, 1, 0) != 1)
            return -1;
        if (c == '\n')
            break;
        if (c != '\r')
            buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static void do_run(SOCKET s, const char *cmdline)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE rd, wr, nul;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    char *cl, buf[4096];
    DWORD got, code = 255;

    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        sendall(s, "\n@@EXIT 255\n", 12);
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFile("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, NULL);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;

    cl = (char *)malloc(strlen(cmdline) + 16);
    strcpy(cl, "cmd /c ");
    strcat(cl, cmdline);
    if (CreateProcess(NULL, cl, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(wr);
        wr = NULL;
        while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got > 0)
            if (sendall(s, buf, (int)got) < 0)
                break;
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        sprintf(buf, "CreateProcess failed: %lu\n", GetLastError());
        sendall(s, buf, strlen(buf));
    }
    free(cl);
    if (wr)
        CloseHandle(wr);
    CloseHandle(rd);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    sprintf(buf, "\n@@EXIT %lu\n", code);
    sendall(s, buf, strlen(buf));
}

static void do_spawn(SOCKET s, const char *cmdline)
{
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    char *cl = (char *)malloc(strlen(cmdline) + 1), buf[64];

    strcpy(cl, cmdline);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcess(NULL, cl, NULL, NULL, FALSE,
                      CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP,
                      NULL, NULL, &si, &pi)) {
        sprintf(buf, "OK %lu\n", pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else
        sprintf(buf, "ERR %lu\n", GetLastError());
    free(cl);
    sendall(s, buf, strlen(buf));
}

static void do_put(SOCKET s, char *args)
{
    char *sp = strrchr(args, ' '), buf[8192];
    long size, left;
    HANDLE f;
    DWORD wrote;
    int ok = 1;

    if (!sp)
        return;
    *sp = 0;
    size = atol(sp + 1);
    f = CreateFile(args, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        ok = 0;
    for (left = size; left > 0;) {
        int k = recv(s, buf, left < (long)sizeof buf ? (int)left : (int)sizeof buf, 0);
        if (k <= 0)
            break;
        if (ok && (!WriteFile(f, buf, k, &wrote, NULL) || wrote != (DWORD)k))
            ok = 0;
        left -= k;
    }
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (ok && left == 0)
        sendall(s, "OK\n", 3);
    else {
        sprintf(buf, "ERR %lu\n", GetLastError());
        sendall(s, buf, strlen(buf));
    }
}

static void do_get(SOCKET s, const char *path)
{
    char buf[8192];
    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got;

    if (f == INVALID_HANDLE_VALUE) {
        sendall(s, "-1\n", 3);
        return;
    }
    size = GetFileSize(f, NULL);
    sprintf(buf, "%lu\n", size);
    sendall(s, buf, strlen(buf));
    while (ReadFile(f, buf, sizeof buf, &got, NULL) && got > 0)
        if (sendall(s, buf, (int)got) < 0)
            break;
    CloseHandle(f);
}

int main(int argc, char **argv)
{
    WSADATA wd;
    SOCKET ls, s;
    struct sockaddr_in sin;
    static char line[4096];
    int one = 1, port = argc > 1 ? atoi(argv[1]) : 2370;

    if (WSAStartup(MAKEWORD(1, 1), &wd) != 0)
        return 1;
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(ls, 4) != 0) {
        printf("ntexec: cannot listen on %d (%d)\n", port, WSAGetLastError());
        return 1;
    }
    printf("ntexec: listening on %d\n", port);
    for (;;) {
        s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET)
            continue;
        if (recvline(s, line, sizeof line) > 0) {
            if (!strncmp(line, "RUN ", 4))
                do_run(s, line + 4);
            else if (!strncmp(line, "SPAWN ", 6))
                do_spawn(s, line + 6);
            else if (!strncmp(line, "PUT ", 4))
                do_put(s, line + 4);
            else if (!strncmp(line, "GET ", 4))
                do_get(s, line + 4);
        }
        shutdown(s, 1);
        closesocket(s);
    }
}
