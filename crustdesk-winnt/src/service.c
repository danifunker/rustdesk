#include "service.h"
#include "agent.h"
#include "log.h"
#include "prefs.h"

#include "win.h"
#include <stdio.h>
#include <string.h>

static void say(char *err, size_t cap, const char *what)
{
    DWORD e = GetLastError();
    char msg[200];
    int n;
    msg[0] = 0;
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg,
                  sizeof msg, NULL);
    for (n = (int)strlen(msg); n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n'); n--)
        msg[n - 1] = 0;
    if (err)
        snprintf(err, cap, "%s: %s (%lu)", what, msg[0] ? msg : "failed", e);
}

static SC_HANDLE open_svc(DWORD access, SC_HANDLE *scm)
{
    SC_HANDLE s;
    *scm = OpenSCManager(NULL, NULL, access & (SERVICE_CHANGE_CONFIG | DELETE)
                                         ? SC_MANAGER_ALL_ACCESS : SC_MANAGER_CONNECT);
    if (!*scm)
        return NULL;
    s = OpenService(*scm, SERVICE_NAME, access);
    if (!s) {
        DWORD e = GetLastError();
        CloseServiceHandle(*scm);
        *scm = NULL;
        SetLastError(e);
    }
    return s;
}

int service_state(void)
{
    SC_HANDLE scm, s = open_svc(SERVICE_QUERY_STATUS, &scm);
    SERVICE_STATUS st;
    int r = SVC_UNKNOWN;
    if (!s)
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? SVC_ABSENT : SVC_UNKNOWN;
    if (QueryServiceStatus(s, &st))
        r = (int)st.dwCurrentState;
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return r;
}

int service_path(char *out, size_t cap)
{
    SC_HANDLE scm, s = open_svc(SERVICE_QUERY_CONFIG, &scm);
    char buf[4096];
    QUERY_SERVICE_CONFIG *qc = (QUERY_SERVICE_CONFIG *)buf;
    DWORD need;
    int ok = 0;
    if (!s)
        return 0;
    if (QueryServiceConfig(s, qc, sizeof buf, &need)) {
        const char *p = qc->lpBinaryPathName, *e;
        if (*p == '"') {
            p++;
            e = strchr(p, '"');
        } else {
            e = strchr(p, ' ');
        }
        if (!e)
            e = p + strlen(p);
        if ((size_t)(e - p) < cap) {
            memcpy(out, p, (size_t)(e - p));
            out[e - p] = 0;
            ok = 1;
        }
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

int service_install(const char *exe, char *err, size_t cap)
{
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS), s;
    char cmd[MAX_PATH + 32];
    if (!scm) {
        say(err, cap, "cannot reach the service manager (are you an Administrator?)");
        return 0;
    }
    snprintf(cmd, sizeof cmd, "\"%s\" -service", exe);
    s = CreateService(scm, SERVICE_NAME, SERVICE_DISPLAY, SERVICE_ALL_ACCESS,
                      SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS, SERVICE_AUTO_START,
                      SERVICE_ERROR_NORMAL, cmd, NULL, NULL, "Tcpip\0\0", NULL, NULL);
    if (!s && GetLastError() == ERROR_SERVICE_EXISTS) {
        /* already there: point it at this program */
        s = OpenService(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
        if (s && !ChangeServiceConfig(s, SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                                      SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL,
                                      "Tcpip\0\0", NULL, NULL, SERVICE_DISPLAY)) {
            say(err, cap, "cannot change the service");
            CloseServiceHandle(s);
            s = NULL;
        }
    } else if (!s) {
        say(err, cap, "cannot install the service");
    }
    if (s)
        CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return s != NULL;
}

static int wait_for(SC_HANDLE s, DWORD pending, DWORD want)
{
    SERVICE_STATUS st;
    int i;
    for (i = 0; i < 80; i++) {
        if (!QueryServiceStatus(s, &st))
            return 0;
        if (st.dwCurrentState == want)
            return 1;
        if (st.dwCurrentState != pending && i > 4)
            return 0;
        Sleep(250);
    }
    return 0;
}

int service_start(char *err, size_t cap)
{
    SC_HANDLE scm, s = open_svc(SERVICE_START | SERVICE_QUERY_STATUS, &scm);
    int ok;
    if (!s) {
        say(err, cap, "cannot open the service");
        return 0;
    }
    ok = StartService(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    if (!ok)
        say(err, cap, "cannot start the service");
    else if (!(ok = wait_for(s, SERVICE_START_PENDING, SERVICE_RUNNING)) && err)
        snprintf(err, cap, "the service did not start; see C-Desk-Vint.log");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

int service_stop(char *err, size_t cap)
{
    SC_HANDLE scm, s = open_svc(SERVICE_STOP | SERVICE_QUERY_STATUS, &scm);
    SERVICE_STATUS st;
    int ok;
    if (!s) {
        say(err, cap, "cannot open the service");
        return 0;
    }
    ok = ControlService(s, SERVICE_CONTROL_STOP, &st) ||
         GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    if (!ok)
        say(err, cap, "cannot stop the service");
    else if (!(ok = wait_for(s, SERVICE_STOP_PENDING, SERVICE_STOPPED)) && err)
        snprintf(err, cap, "the service did not stop");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

int service_remove(char *err, size_t cap)
{
    SC_HANDLE scm, s;
    int ok;
    service_stop(NULL, 0);
    s = open_svc(DELETE, &scm);
    if (!s) {
        say(err, cap, "cannot open the service");
        return 0;
    }
    ok = DeleteService(s) != 0;
    if (!ok)
        say(err, cap, "cannot remove the service");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

/* ---- running as the service -------------------------------------------------- */

static SERVICE_STATUS_HANDLE status_h;
static SERVICE_STATUS status;
static HANDLE stop_ev;
static int svc_verbose;

static void report(DWORD state, DWORD exitcode, DWORD wait_hint)
{
    static DWORD check = 1;
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_START_PENDING
                                    ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = exitcode;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : check++;
    status.dwWaitHint = wait_hint;
    SetServiceStatus(status_h, &status);
}

static VOID WINAPI handler(DWORD ctl)
{
    if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING, NO_ERROR, 10000);
        SetEvent(stop_ev);
        return;
    }
    report(status.dwCurrentState, NO_ERROR, 0);
}

static VOID WINAPI service_main(DWORD argc, LPSTR *argv)
{
    int r;
    (void)argc;
    (void)argv;
    status_h = RegisterServiceCtrlHandler(SERVICE_NAME, handler);
    if (!status_h)
        return;
    stop_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    report(SERVICE_START_PENDING, NO_ERROR, 5000);
    report(SERVICE_RUNNING, NO_ERROR, 0);
    r = agent_run(stop_ev, 0, svc_verbose);
    report(SERVICE_STOPPED, r ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR, 0);
}

int service_dispatch(int verbose)
{
    SERVICE_TABLE_ENTRY t[2];
    svc_verbose = verbose;
    t[0].lpServiceName = SERVICE_NAME;
    t[0].lpServiceProc = service_main;
    t[1].lpServiceName = NULL;
    t[1].lpServiceProc = NULL;
    return StartServiceCtrlDispatcher(t) != 0;
}
