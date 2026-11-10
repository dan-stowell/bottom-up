// prof.c: poor man's profiler (this VM has no perf/gdb).
//   prof count  INPUT PROG   -> exact per-address executed-instruction counts (ptrace single-step)
//   prof sample INPUT PROG N -> per-address sample counts over N runs (PTRACE_INTERRUPT every ~30us)
// Output: "addr count" lines on stdout.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define HS (1<<22)
static unsigned long long keys[HS]; static unsigned long long vals[HS];
static void bump(unsigned long long a){ unsigned long long h=(a*0x9E3779B97F4A7C15ull)>>42; while(keys[h]&&keys[h]!=a) h=(h+1)&(HS-1); keys[h]=a; vals[h]++; }
static pid_t spawn(const char*in,char**argv,int seize){ pid_t p=fork(); if(!p){ int fd=open(in,O_RDONLY); dup2(fd,0); int o=open("/dev/null",O_WRONLY); dup2(o,1);
    if(!seize) ptrace(PTRACE_TRACEME,0,0,0); else raise(SIGSTOP); execv(argv[0],argv); _exit(127);} return p; }
int main(int argc,char**argv){ struct user_regs_struct r; int st;
  if(!strcmp(argv[1],"count")){ pid_t p=spawn(argv[2],argv+3,0); waitpid(p,&st,0);
    for(;;){ ptrace(PTRACE_GETREGS,p,0,&r); bump(r.rip); if(ptrace(PTRACE_SINGLESTEP,p,0,0)<0) break; waitpid(p,&st,0); if(!WIFSTOPPED(st)) break; }
  } else { int n=atoi(argv[4]); char*av[]={argv[3],0};
    for(int i=0;i<n;i++){ pid_t p=fork(); if(!p){ int fd=open(argv[2],O_RDONLY); dup2(fd,0); int o=open("/dev/null",O_WRONLY); dup2(o,1); ptrace(PTRACE_TRACEME,0,0,0); execv(av[0],av); _exit(127);} 
      waitpid(p,&st,0); ptrace(PTRACE_CONT,p,0,0);
      for(;;){ struct timespec ts={0,20000+ (rand()%20000)}; nanosleep(&ts,0); if(kill(p,SIGSTOP)<0) break; waitpid(p,&st,0); if(!WIFSTOPPED(st)) break;
        ptrace(PTRACE_GETREGS,p,0,&r); bump(r.rip); ptrace(PTRACE_CONT,p,0,0); }
    } }
  for(int i=0;i<HS;i++) if(keys[i]) printf("%llx %llu\n",keys[i],vals[i]);
  return 0; }
