/* C-Desk-Vint for Windows NT: one program, several ways in.
 *
 *   C-Desk-Vint              the settings window (and installing)
 *   C-Desk-Vint -service     what the service manager runs
 *   C-Desk-Vint -tray        the icon by the clock and chat, as the user
 *                            logged on (started at logon on NT)
 *   C-Desk-Vint -run [-v]    the agent in a console window, for testing
 *   C-Desk-Vint -install     install and start the service (this program)
 *   C-Desk-Vint -remove      stop and remove it
 *   C-Desk-Vint -start | -stop
 *   C-Desk-Vint -i           this machine's ID and password
 *   C-Desk-Vint -selftest    check the compiler got the core right
 */
#include "agent.h"
#include "prefs.h"
#include "selftest.h"
#include "service.h"
#include "tray.h"
#include "ui.h"

#include "win.h"
#include <stdio.h>
#include <string.h>

/* Text for whoever started us: a console or a pipe if there is one (a GUI
 * program inherits handles but gets no console of its own), else a box. */
static void out(const char *text)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w;
    if (h && h != INVALID_HANDLE_VALUE && WriteFile(h, text, (DWORD)strlen(text), &w, NULL))
        return;
    MessageBox(NULL, text, "C-Desk-Vint", MB_OK);
}

static HANDLE stop_ev;

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    SetEvent(stop_ev);
    return TRUE;
}

static int has(const char *cmd, const char *opt)
{
    size_t n = strlen(opt);
    const char *p = cmd;
    while ((p = strstr(p, opt)) != NULL) {
        if ((p == cmd || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return 1;
        p += n;
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char err[300], buf[600], exe[MAX_PATH];
    int verbose = has(cmd, "-v");
    (void)prev;
    (void)show;
    if (has(cmd, "-service"))
        return service_dispatch(verbose) ? 0 : 1;
    if (has(cmd, "-tray"))
        return tray_run(inst);
    if (has(cmd, "-run")) {
        int r;
        if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL ||
            GetStdHandle(STD_OUTPUT_HANDLE) == INVALID_HANDLE_VALUE)
            AllocConsole();
        stop_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
        SetConsoleCtrlHandler(on_ctrl, TRUE);
        r = agent_run(stop_ev, 1, verbose);
        return r;
    }
    if (has(cmd, "-selftest"))
        return selftest(out);
    if (has(cmd, "-i")) {
        char id[32];
        if (!prefs_load(1)) {
            out("cannot write the settings\r\n");
            return 1;
        }
        prefs_format_id(prefs.id, id, sizeof id);
        snprintf(buf, sizeof buf, "ID: %s\r\nPassword: %s\r\nID server: %s\r\nDirect access: %s%d\r\n"
                 "Version: %s\r\n", id, prefs.password, prefs.server[0] ? prefs.server : "(none)",
                 prefs.port ? "port " : "off", prefs.port, CDV_BUILD);
        out(buf);
        return 0;
    }
    if (has(cmd, "-install")) {
        GetModuleFileName(NULL, exe, sizeof exe);
        prefs_load(1);
        if (!service_install(exe, err, sizeof err) || !service_start(err, sizeof err)) {
            snprintf(buf, sizeof buf, "%s\r\n", err);
            out(buf);
            return 1;
        }
        service_tray_now(exe);
        out("installed and started\r\n");
        return 0;
    }
    if (has(cmd, "-remove")) {
        if (!service_remove(err, sizeof err)) {
            snprintf(buf, sizeof buf, "%s\r\n", err);
            out(buf);
            return 1;
        }
        out("removed\r\n");
        return 0;
    }
    if (has(cmd, "-start") || has(cmd, "-stop")) {
        int ok = has(cmd, "-start") ? service_start(err, sizeof err) : service_stop(err, sizeof err);
        snprintf(buf, sizeof buf, "%s\r\n", ok ? "done" : err);
        out(buf);
        return !ok;
    }
    return ui_run(inst);
}
