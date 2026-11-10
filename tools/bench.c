// bench.c: run "prog < file > /dev/null" N times; report wall time (min, median) and the CPU time,
// and page faults of the fastest run. Any failed open/fork/exec/wait or nonzero child status is an error
// (AUDIT.md, defect 1: a crashed child used to be reported as a very fast benchmark).
//   bench N FILE PROG [ARGS...]
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
int main(int argc,char**argv){
  if(argc<4){ fprintf(stderr,"usage: bench N FILE PROG [ARGS]\n"); return 2; }
  int n=atoi(argv[1]); if(n<1) return 2;
  double *w=malloc(n*sizeof*w),*s=malloc(n*sizeof*s); double busr=0,bsys=0,best=1e300; long bflt=0;
  for(int i=0;i<n;i++){ double t0=now(); pid_t p=fork();
    if(p<0){ perror("fork"); return 1; }
    if(!p){ int fd=open(argv[2],O_RDONLY); if(fd<0||dup2(fd,0)<0) _exit(125);
      int o=open("/dev/null",O_WRONLY); if(o<0||dup2(o,1)<0) _exit(126);
      execv(argv[3],argv+3); _exit(127); }
    int st=0; struct rusage ru;
    if(wait4(p,&st,0,&ru)!=p||!WIFEXITED(st)||WEXITSTATUS(st)!=0){ fprintf(stderr,"bench: %s failed (wait status %d)\n",argv[3],st); return 1; }
    w[i]=s[i]=now()-t0;
    if(w[i]<best){ best=w[i]; busr=ru.ru_utime.tv_sec*1e6+ru.ru_utime.tv_usec; bsys=ru.ru_stime.tv_sec*1e6+ru.ru_stime.tv_usec; bflt=ru.ru_minflt; } }
  qsort(s,n,sizeof*s,cmp);
  printf("wall min %.0f us  median %.0f us   fastest run: usr %.0f sys %.0f faults %ld   all:",s[0],s[n/2],busr,bsys,bflt);
  for(int i=0;i<n;i++) printf(" %.0f",w[i]);
  printf("\n"); return 0; }
