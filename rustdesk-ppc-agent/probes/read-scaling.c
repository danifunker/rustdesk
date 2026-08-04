#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
static double now(void){struct timeval t;gettimeofday(&t,0);return t.tv_sec+t.tv_usec/1e6;}
static volatile unsigned long sink;   /* defeat dead-code elimination */
int main(void){
    CGDirectDisplayID d=CGMainDisplayID();
    size_t h=CGDisplayPixelsHigh(d),bpr=CGDisplayBytesPerRow(d);
    unsigned char*fb=CGDisplayBaseAddress(d);
    unsigned char*ram=malloc(bpr*h);
    double t; unsigned long s;

    t=now(); memcpy(ram,fb,bpr*h); s=0; for(size_t i=0;i<bpr*h;i+=4096)s+=ram[i]; sink=s;
    printf("full frame (%.1f MB)   : %6.1f ms\n",bpr*h/1048576.0,(now()-t)*1000);

    for (int N=2;N<=32;N*=2){
        t=now();
        for(size_t r=0;r<h;r+=N) memcpy(ram+r*bpr, fb+r*bpr, bpr);
        s=0; for(size_t r=0;r<h;r+=N) s+=ram[r*bpr]; sink=s;
        printf("every %2dth row (1/%2d)  : %6.1f ms\n",N,N,(now()-t)*1000);
    }
    /* contiguous band = what a dirty-rect capture would actually read */
    for (int frac=2;frac<=16;frac*=2){
        size_t rows=h/frac;
        t=now(); memcpy(ram, fb, rows*bpr);
        s=0; for(size_t i=0;i<rows*bpr;i+=4096)s+=ram[i]; sink=s;
        printf("top 1/%-2d contiguous   : %6.1f ms\n",frac,(now()-t)*1000);
    }
    return 0;
}
