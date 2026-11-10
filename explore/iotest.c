// iotest.c -- what does it cost just to *get at* a 256 MiB page-cache file, per strategy?
// touch one byte per page (or read into a reused buffer), T threads. Prints ms.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec*1e-6; }
static unsigned char*base; static size_t per; static int fd; static volatile unsigned long sink;
static void* touch(void*a){ long t=(long)a; unsigned long s=0; for(size_t i=0;i<per;i+=4096) s+=base[t*per+i]; sink+=s; return 0; }
static void* rd(void*a){ long t=(long)a; static __thread unsigned char buf[1<<18]; unsigned long s=0; for(size_t o=0;o<per;o+=sizeof buf){ ssize_t n=pread(fd,buf,sizeof buf,t*per+o); s+=buf[0]+n; } sink+=s; return 0; }
static double par(void*(*f)(void*),int T,size_t n){ pthread_t th[16]; per=n/T; double t0=now(); for(long t=0;t<T;t++) pthread_create(&th[t],0,f,(void*)t); for(int t=0;t<T;t++) pthread_join(th[t],0); return now()-t0; }
int main(int c,char**v){ fd=open(v[1],O_RDONLY); struct stat st; fstat(fd,&st); size_t n=st.st_size; int Ts[]={1,2,4,8};
  printf("| strategy | 1 thread | 2 | 4 | 8 |\n|---|---|---|---|---|\n");
  printf("| mmap, fault on touch |"); for(int i=0;i<4;i++){ double t0=now(); base=mmap(0,n,PROT_READ,MAP_PRIVATE,fd,0); double m=now()-t0; double t=par(touch,Ts[i],n); t0=now(); munmap(base,n); printf(" %.1f (+%.1f unmap) |",m+t,now()-t0);} printf("\n");
  printf("| mmap MAP_POPULATE (serial prefault) |"); for(int i=0;i<4;i++){ double t0=now(); base=mmap(0,n,PROT_READ,MAP_PRIVATE|MAP_POPULATE,fd,0); double m=now()-t0; double t=par(touch,Ts[i],n); t0=now(); munmap(base,n); printf(" %.1f (+%.1f unmap) |",m+t,now()-t0);} printf("\n");
  printf("| pread into a reused 256 KiB buffer |"); for(int i=0;i<4;i++) printf(" %.1f |",par(rd,Ts[i],n)); printf("\n");
  return 0; }
