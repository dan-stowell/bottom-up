// bench.c: run "prog < file" N times, report min/median wall time and min user+sys CPU time.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e6+t.tv_nsec/1e3; }
static int cmp(const void*a,const void*b){ double x=*(double*)a,y=*(double*)b; return x<y?-1:x>y; }
int main(int argc,char**argv){ int n=atoi(argv[1]); double *w=malloc(n*sizeof*w),*c=malloc(n*sizeof*c),*u=malloc(n*sizeof*u),*sy=malloc(n*sizeof*sy); long flt=0;
  for(int i=0;i<n;i++){ double t0=now(); pid_t p=fork();
    if(!p){ int fd=open(argv[2],O_RDONLY); dup2(fd,0); int o=open("/dev/null",O_WRONLY); dup2(o,1); execv(argv[3],argv+3); _exit(127);} 
    int st; struct rusage ru; wait4(p,&st,0,&ru); w[i]=now()-t0; u[i]=ru.ru_utime.tv_sec*1e6+ru.ru_utime.tv_usec; sy[i]=ru.ru_stime.tv_sec*1e6+ru.ru_stime.tv_usec; flt=ru.ru_minflt; c[i]=ru.ru_utime.tv_sec*1e6+ru.ru_utime.tv_usec+ru.ru_stime.tv_sec*1e6+ru.ru_stime.tv_usec; }
  qsort(w,n,sizeof*w,cmp); qsort(c,n,sizeof*c,cmp); qsort(u,n,sizeof*u,cmp); qsort(sy,n,sizeof*sy,cmp);
  printf("wall min %.0f us  median %.0f us   cpu min %.0f us median %.0f us   usr min %.0f sys min %.0f median usr %.0f sys %.0f faults %ld\n",w[0],w[n/2],c[0],c[n/2],u[0],sy[0],u[n/2],sy[n/2],flt); return 0; }
