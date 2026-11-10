// base.c -- scalar C baselines as whole programs: read stdin in 64 KiB blocks.
// -DK=1 count newlines, -DK=2 count words, -DK=3 uppercase to stdout. Prints/writes like the Strand versions.
#include <stdint.h>
#include <unistd.h>
static uint8_t in[1<<16], out[1<<16];
int main(void){ uint64_t c=0; int prev=1; ssize_t n;
  while((n=read(0,in,sizeof in))>0){
#if K==1
    for(ssize_t i=0;i<n;i++) c+=in[i]==10;
#elif K==2
    for(ssize_t i=0;i<n;i++){ int b=in[i]<=32; c+=(!b)&prev; prev=b; }
#else
    for(ssize_t i=0;i<n;i++){ uint8_t x=in[i]; out[i]=x-(((x>=97)&(x<=122))<<5); }
    for(ssize_t o=0;o<n;){ ssize_t w=write(1,out+o,n-o); if(w<=0) return 1; o+=w; }
#endif
  }
#if K!=3
  uint32_t r=c; write(1,&r,4);
#endif
  return 0; }
