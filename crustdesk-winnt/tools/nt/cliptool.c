/* cliptool: the clipboard from the command line, for testing the agent.
 *
 *   cliptool get          print the clipboard's text, as UTF-8 hex (so a
 *                         pipe through ntexec carries it unchanged)
 *   cliptool set HEX      put UTF-8 text, given as hex, on the clipboard
 *
 * On Windows 95/98, whose clipboard keeps text in the code page, CF_TEXT.
 *
 * Build on NT:  cl cliptool.c user32.lib
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CP_UTF8
#define CP_UTF8 65001 /* NT 4.0 knows it; Visual C++ 4's headers do not */
#endif

int main(int argc, char **argv)
{
    HWND w = CreateWindow("STATIC", "", 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    if (argc >= 2 && !strcmp(argv[1], "get")) {
        HANDLE h;
        const WCHAR *t;
        char *u;
        int n, i;
        WCHAR *conv = NULL;
        int nt = GetVersion() < 0x80000000;
        if (!OpenClipboard(w))
            return 1;
        h = GetClipboardData(nt ? CF_UNICODETEXT : CF_TEXT);
        t = h ? (const WCHAR *)GlobalLock(h) : NULL;
        if (!t) {
            CloseClipboard();
            printf("(no text)\n");
            return 1;
        }
        if (!nt) {
            int wn = MultiByteToWideChar(CP_ACP, 0, (const char *)t, -1, NULL, 0);
            conv = (WCHAR *)malloc(wn * sizeof(WCHAR));
            MultiByteToWideChar(CP_ACP, 0, (const char *)t, -1, conv, wn);
            t = conv;
        }
        n = WideCharToMultiByte(CP_UTF8, 0, t, -1, NULL, 0, NULL, NULL);
        u = (char *)malloc(n);
        WideCharToMultiByte(CP_UTF8, 0, t, -1, u, n, NULL, NULL);
        for (i = 0; u[i]; i++)
            printf("%02x", (unsigned char)u[i]);
        printf("\n");
        GlobalUnlock(h);
        CloseClipboard();
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        size_t n = strlen(argv[2]) / 2, i;
        char *u = (char *)malloc(n + 1);
        WCHAR *t;
        HGLOBAL g;
        int wn;
        for (i = 0; i < n; i++) {
            unsigned v;
            sscanf(argv[2] + 2 * i, "%2x", &v);
            u[i] = (char)v;
        }
        u[n] = 0;
        wn = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
        g = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, wn * sizeof(WCHAR));
        t = (WCHAR *)GlobalLock(g);
        MultiByteToWideChar(CP_UTF8, 0, u, -1, t, wn);
        GlobalUnlock(g);
        if (GetVersion() >= 0x80000000) {
            /* 9x: the code page's text */
            int an = wn * 2; /* a DBCS code page takes at most two bytes a character */
            HGLOBAL ga = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, an);
            WideCharToMultiByte(CP_ACP, 0, (WCHAR *)GlobalLock(g), -1, (char *)GlobalLock(ga), an,
                                NULL, NULL);
            GlobalUnlock(ga);
            GlobalUnlock(g);
            g = ga;
        }
        if (!OpenClipboard(w))
            return 1;
        EmptyClipboard();
        SetClipboardData(GetVersion() < 0x80000000 ? CF_UNICODETEXT : CF_TEXT, g);
        CloseClipboard();
        return 0;
    }
    fprintf(stderr, "usage: cliptool get | cliptool set HEX\n");
    return 2;
}
