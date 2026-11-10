// ceiling.c -- how fast could Strand-shaped byte programs go on this machine?
// Three kernels (count newlines; count words = wc -w; uppercase into an output buffer), each as:
//   scalar      plain C loop, gcc -O2 (no vectorisation)
//   autovec     the same loop, gcc -O3 -march=native
//   i32x16      AVX-512 the way Strand works today: widen bytes to 16 dword lanes
//   u8x64       AVX-512 with 64 byte lanes
//   u8x64 x T   the byte-lane kernel on T threads
// Memory is pre-touched; this measures steady-state throughput, not page faults.
// build: gcc -O2 -pthread -o ceiling ceiling.c   (kernels carry their own target attributes)
#define _GNU_SOURCE
#include <immintrin.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
#define NOVEC __attribute__((noinline,optimize("O2","no-tree-vectorize")))
#define AUTOV __attribute__((noinline,optimize("O3"),target("avx512f,avx512bw,avx512vl,avx512dq,bmi2,popcnt")))
#define A512  __attribute__((noinline,target("avx512f,avx512bw,avx512vl,avx512dq,avx512vbmi,bmi2,popcnt")))
static inline int blank(uint8_t c){ return c<=32; }

NOVEC static uint64_t nl_scalar(const uint8_t*p,size_t n,uint8_t*o){ uint64_t c=0; for(size_t i=0;i<n;i++) c+=p[i]==10; return c; }
AUTOV static uint64_t nl_autovec(const uint8_t*p,size_t n,uint8_t*o){ uint64_t c=0; for(size_t i=0;i<n;i++) c+=p[i]==10; return c; }
A512 static uint64_t nl_i32(const uint8_t*p,size_t n,uint8_t*o){ __m512i acc=_mm512_setzero_si512(),ten=_mm512_set1_epi32(10),one=_mm512_set1_epi32(1);
  for(size_t i=0;i<n;i+=16){ __m512i x=_mm512_cvtepu8_epi32(_mm_loadu_si128((void*)(p+i))); acc=_mm512_add_epi32(acc,_mm512_maskz_mov_epi32(_mm512_cmpeq_epi32_mask(x,ten),one)); } return (uint32_t)_mm512_reduce_add_epi32(acc); }
A512 static uint64_t nl_u8(const uint8_t*p,size_t n,uint8_t*o){ uint64_t c=0; __m512i ten=_mm512_set1_epi8(10); for(size_t i=0;i<n;i+=64) c+=_mm_popcnt_u64(_mm512_cmpeq_epi8_mask(_mm512_loadu_si512(p+i),ten)); return c; }

NOVEC static uint64_t wc_scalar(const uint8_t*p,size_t n,uint8_t*o){ uint64_t c=0; int prev=1; for(size_t i=0;i<n;i++){ int b=blank(p[i]); c+=(!b)&prev; prev=b; } return c; }
AUTOV static uint64_t wc_autovec(const uint8_t*p,size_t n,uint8_t*o){ uint64_t c=0; int prev=1; for(size_t i=0;i<n;i++){ int b=blank(p[i]); c+=(!b)&prev; prev=b; } return c; }
A512 static uint64_t wc_i32(const uint8_t*p,size_t n,uint8_t*o){ __m512i acc=_mm512_setzero_si512(),sp=_mm512_set1_epi32(32),one=_mm512_set1_epi32(1),carry=one;
  for(size_t i=0;i<n;i+=16){ __m512i x=_mm512_cvtepu8_epi32(_mm_loadu_si128((void*)(p+i))); __m512i b=_mm512_maskz_mov_epi32(_mm512_cmple_epi32_mask(x,sp),one);
    __m512i pb=_mm512_alignr_epi32(b,carry,15); carry=b; acc=_mm512_add_epi32(acc,_mm512_andnot_si512(b,pb)); } return (uint32_t)_mm512_reduce_add_epi32(acc); }
A512 static uint64_t wc_u8(const uint8_t*p,size_t n,uint8_t*o){ uint64_t c=0,carry=1; __m512i sp=_mm512_set1_epi8(32);
  for(size_t i=0;i<n;i+=64){ uint64_t b=_mm512_cmple_epu8_mask(_mm512_loadu_si512(p+i),sp); c+=_mm_popcnt_u64(~b&((b<<1)|carry)); carry=b>>63; } return c; }

NOVEC static uint64_t up_scalar(const uint8_t*p,size_t n,uint8_t*o){ for(size_t i=0;i<n;i++){ uint8_t c=p[i]; o[i]=c-(((c>=97)&(c<=122))<<5); } return o[n/2]; }
AUTOV static uint64_t up_autovec(const uint8_t*p,size_t n,uint8_t*o){ for(size_t i=0;i<n;i++){ uint8_t c=p[i]; o[i]=c-(((c>=97)&(c<=122))<<5); } return o[n/2]; }
A512 static uint64_t up_i32(const uint8_t*p,size_t n,uint8_t*o){ __m512i a=_mm512_set1_epi32(97),z=_mm512_set1_epi32(122),d=_mm512_set1_epi32(32);
  for(size_t i=0;i<n;i+=16){ __m512i x=_mm512_cvtepu8_epi32(_mm_loadu_si128((void*)(p+i))); __mmask16 k=_mm512_cmpge_epi32_mask(x,a)&_mm512_cmple_epi32_mask(x,z);
    _mm_storeu_si128((void*)(o+i),_mm512_cvtepi32_epi8(_mm512_mask_sub_epi32(x,k,x,d))); } return o[n/2]; }
A512 static uint64_t up_u8(const uint8_t*p,size_t n,uint8_t*o){ __m512i a=_mm512_set1_epi8(97),z=_mm512_set1_epi8(122),d=_mm512_set1_epi8(32);
  for(size_t i=0;i<n;i+=64){ __m512i x=_mm512_loadu_si512(p+i); __mmask64 k=_mm512_cmpge_epu8_mask(x,a)&_mm512_cmple_epu8_mask(x,z); _mm512_storeu_si512(o+i,_mm512_mask_sub_epi8(x,k,x,d)); } return o[n/2]; }

typedef uint64_t (*kern)(const uint8_t*,size_t,uint8_t*);
struct job { kern f; const uint8_t*p; size_t n; uint8_t*o; uint64_t r; };
static void* run(void*a){ struct job*j=a; j->r=j->f(j->p,j->n,j->o); return 0; }
static uint64_t threaded(kern f,const uint8_t*p,size_t n,uint8_t*o,int T){ pthread_t th[64]; struct job j[64]; size_t per=(n/T)&~(size_t)63; uint64_t r=0;
  for(int t=0;t<T;t++){ j[t]=(struct job){f,p+per*t,t==T-1?n-per*t:per,o+per*t,0}; pthread_create(&th[t],0,run,&j[t]); }
  for(int t=0;t<T;t++){ pthread_join(th[t],0); r+=j[t].r; } return r; }   // (word counts at seams are approximate; this is a throughput probe)

int main(int argc,char**argv){ size_t n=(size_t)(argc>1?atoi(argv[1]):256)<<20; uint8_t*p=aligned_alloc(4096,n+4096),*o=aligned_alloc(4096,n+4096);
  uint32_t s=12345; for(size_t i=0;i<n;i++){ s=s*1664525+1013904223; uint32_t r=s>>24; p[i]= r<40?32: r<46?10: 97+(r%26); } memset(o,1,n);
  struct { const char*name; kern f[4]; } K[]={{"count newlines",{nl_scalar,nl_autovec,nl_i32,nl_u8}},{"count words (prev)",{wc_scalar,wc_autovec,wc_i32,wc_u8}},{"uppercase -> buffer",{up_scalar,up_autovec,up_i32,up_u8}}};
  const char*V[]={"scalar -O2","gcc -O3 autovec","AVX-512 i32x16 (Strand today)","AVX-512 u8x64"};
  printf("input %zu MiB, in memory, pre-touched; best of 5; GB/s\n| kernel | %s | %s | %s | %s | u8x64 x2 | x4 | x8 |\n|---|---|---|---|---|---|---|---|\n",n>>20,V[0],V[1],V[2],V[3]);
  for(int k=0;k<3;k++){ printf("| %s |",K[k].name);
    for(int v=0;v<4;v++){ double best=1e9; uint64_t r=0; for(int it=0;it<5;it++){ double t=now(); r=K[k].f[v](p,n,o); t=now()-t; if(t<best)best=t; } printf(" %.2f |",n/best/1e9); (void)r; }
    for(int T=2;T<=8;T*=2){ double best=1e9; for(int it=0;it<5;it++){ double t=now(); threaded(K[k].f[3],p,n,o,T); t=now()-t; if(t<best)best=t; } printf(" %.1f |",n/best/1e9); }
    printf("\n"); }
  printf("check: newlines %llu %llu %llu %llu ; words %llu %llu %llu %llu\n",(unsigned long long)nl_scalar(p,n,o),(unsigned long long)nl_autovec(p,n,o),(unsigned long long)nl_i32(p,n,o),(unsigned long long)nl_u8(p,n,o),
     (unsigned long long)wc_scalar(p,n,o),(unsigned long long)wc_autovec(p,n,o),(unsigned long long)wc_i32(p,n,o),(unsigned long long)wc_u8(p,n,o));
  return 0; }
