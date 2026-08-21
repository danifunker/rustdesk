#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
int main(void){
    Display *d;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("A: before XOpenDisplay\n");
    d = XOpenDisplay(NULL);
    printf("B: XOpenDisplay returned %s\n", d ? "OK" : "NULL");
    if (d) { printf("C: %dx%d depth %d\n",
                    WidthOfScreen(DefaultScreenOfDisplay(d)),
                    HeightOfScreen(DefaultScreenOfDisplay(d)),
                    DefaultDepthOfScreen(DefaultScreenOfDisplay(d)));
             XCloseDisplay(d); printf("D: closed\n"); }
    return 0;
}
