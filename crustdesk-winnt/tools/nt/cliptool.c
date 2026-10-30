/* cliptool: the clipboard from the command line, for testing the agent.
 *
 *   cliptool get          print the clipboard's text, as UTF-8 hex (so a
 *                         pipe through ntexec carries it unchanged)
 *   cliptool set HEX      put UTF-8 text, given as hex, on the clipboard
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
        if (!OpenClipboard(w))
            return 1;
        h = GetClipboardData(CF_UNICODETEXT);
        t = h ? (const WCHAR *)GlobalLock(h) : NULL;
        if (!t) {
            CloseClipboard();
            printf("(no text)\n");
            return 1;
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
        if (!OpenClipboard(w))
            return 1;
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, g);
        CloseClipboard();
        return 0;
    }
    fprintf(stderr, "usage: cliptool get | cliptool set HEX\n");
    return 2;
}
