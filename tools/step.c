// step.c: single-step a program and report where rsp changes (debug aid; no gdb on this VM)
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc,char**argv){ pid_t p=fork(); if(!p){ ptrace(PTRACE_TRACEME,0,0,0); execv(argv[1],argv+1); return 1; }
  int st; waitpid(p,&st,0); struct user_regs_struct r; unsigned long long lastsp=0; long n=0;
  while(1){ ptrace(PTRACE_GETREGS,p,0,&r); if(r.rsp!=lastsp){ printf("step %ld rip=%llx rsp=%llx\n",n,r.rip,r.rsp); lastsp=r.rsp; }
    if(ptrace(PTRACE_SINGLESTEP,p,0,0)<0) break; waitpid(p,&st,0); n++; if(WIFEXITED(st)){ printf("exit %d\n",WEXITSTATUS(st)); break;} if(WIFSTOPPED(st)&&WSTOPSIG(st)!=5){ ptrace(PTRACE_GETREGS,p,0,&r); printf("signal %d at rip=%llx rsp=%llx\n",WSTOPSIG(st),r.rip,r.rsp); break; } }
  return 0; }
