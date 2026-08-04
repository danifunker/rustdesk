#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
int main(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    size_t w = CGDisplayPixelsWide(d), h = CGDisplayPixelsHigh(d);
    size_t bpr = CGDisplayBytesPerRow(d), bpp = CGDisplayBitsPerPixel(d);
    size_t spp = CGDisplaySamplesPerPixel(d), bps = CGDisplayBitsPerSample(d);
    printf("display %ux%u bpr=%u bpp=%u spp=%u bps=%u\n",
        (unsigned)w,(unsigned)h,(unsigned)bpr,(unsigned)bpp,(unsigned)spp,(unsigned)bps);
    void *base = CGDisplayBaseAddress(d);   /* WITHOUT CGDisplayCapture */
    printf("base(no capture) = %p\n", base);
    if (base) {
        unsigned char *p = base;
        printf("first pixels: %02x %02x %02x %02x | %02x %02x %02x %02x\n",
            p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
        /* crude liveness: checksum a row now and again shortly after */
        unsigned long s1=0; for (size_t i=0;i<bpr;i++) s1+=p[i];
        sleep(1);
        unsigned long s2=0; for (size_t i=0;i<bpr;i++) s2+=p[i];
        printf("row checksum t0=%lu t1=%lu (%s)\n", s1, s2, s1==s2?"static":"CHANGING");
    }
    return 0;
}
