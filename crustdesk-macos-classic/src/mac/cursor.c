#include "cursor.h"

#include <string.h>

#define LM_THECRSR ((volatile const uint16_t *)0x0844) /* Cursor: data, mask, hotSpot */
#define LM_MOUSE (*(volatile const long *)0x0830)      /* Point, v then h */

#if defined(__powerpc__) || defined(__ppc__)
#pragma pack(push, 2)
#endif
typedef struct {
    long csCursorX, csCursorY;
    unsigned long csCursorVisible, csCursorSet, csReserved1, csReserved2;
} hw_cursor_state;
#if defined(__powerpc__) || defined(__ppc__)
#pragma pack(pop)
#endif

int cursor_is_hardware(short refnum)
{
    CntrlParam pb;
    hw_cursor_state st;
    void *p = &st;
    memset(&pb, 0, sizeof pb);
    memset(&st, 0, sizeof st);
    pb.ioCRefNum = refnum;
    pb.csCode = 23; /* cscGetHardwareCursorDrawState */
    memcpy(pb.csParam, &p, sizeof p);
    if (PBStatusSync((ParmBlkPtr)&pb) != noErr)
        return 0;
    return st.csCursorSet != 0;
}

uint32_t cursor_shape(uint8_t rgba[16 * 16 * 4], int *hotx, int *hoty)
{
    uint16_t c[34];
    uint32_t sum = 0;
    int x, y;
    for (x = 0; x < 34; x++) {
        c[x] = LM_THECRSR[x];
        sum = sum * 31 + c[x];
    }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            uint8_t *p = rgba + (y * 16 + x) * 4;
            int d = c[y] >> (15 - x) & 1, m = c[16 + y] >> (15 - x) & 1;
            p[0] = p[1] = p[2] = (uint8_t)(d ? 0 : 255);
            p[3] = (uint8_t)(d || m ? 255 : 0);
        }
    *hoty = (int16_t)c[32];
    *hotx = (int16_t)c[33];
    return sum;
}

void cursor_where(int *x, int *y)
{
    long v = LM_MOUSE;
    *y = (int16_t)(v >> 16);
    *x = (int16_t)v;
}
