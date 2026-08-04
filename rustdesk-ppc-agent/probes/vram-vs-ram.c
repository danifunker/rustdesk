#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
static double now(void){struct timeval t;gettimeofday(&t,0);return t.tv_sec+t.tv_usec/1e6;}
int main(void){
    CGDirectDisplayID d=CGMainDisplayID();
    size_t w=CGDisplayPixelsWide(d),h=CGDisplayPixelsHigh(d),bpr=CGDisplayBytesPerRow(d);
    unsigned char*fb=CGDisplayBaseAddress(d);
    size_t bytes=bpr*h;
    unsigned char*ram=malloc(bytes);
    printf("frame = %.1f MB\n", bytes/1048576.0);

    double t0=now(); memcpy(ram,fb,bytes); double t1=now();
    printf("VRAM -> RAM memcpy : %6.0f ms  (%.1f MB/s)\n",(t1-t0)*1000, bytes/1048576.0/(t1-t0));

    double t2=now(); memcpy(ram,ram,bytes>1?bytes/2:bytes); double t3=now();
    printf("RAM  -> RAM memcpy : %6.0f ms\n",(t3-t2)*1000);

    /* luma-only conversion out of the RAM copy */
    unsigned char*y=malloc(w*h);
    double t4=now();
    for(size_t r=0;r<h;r++){unsigned char*L=ram+r*bpr;unsigned char*Y=y+r*w;
        for(size_t c=0;c<w;c++){unsigned char*p=L+c*4;
            Y[c]=(unsigned char)(((66*p[1]+129*p[2]+25*p[3]+128)>>8)+16);}}
    double t5=now();
    printf("ARGB->Y from RAM   : %6.0f ms\n",(t5-t4)*1000);

    double t6=now();
    for(size_t r=0;r<h;r++){unsigned char*L=fb+r*bpr;unsigned char*Y=y+r*w;
        for(size_t c=0;c<w;c++){unsigned char*p=L+c*4;
            Y[c]=(unsigned char)(((66*p[1]+129*p[2]+25*p[3]+128)>>8)+16);}}
    double t7=now();
    printf("ARGB->Y from VRAM  : %6.0f ms  <-- reading VRAM per pixel\n",(t7-t6)*1000);
    return 0;
}
