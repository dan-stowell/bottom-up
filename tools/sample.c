// sample.c -- a sampling profiler for a Strand binary, by ptrace: where is the time?  sample PROG < input
// The child runs freely; every 250 microseconds it is interrupted, its instruction pointer is recorded,
// and it continues. Writes "address count" lines to the file named by SAMPLE_OUT (default sample.txt);
// tools/whereis.py attributes them to the program's tokens. One thread: run under `taskset -c N`.
// There is no perf on this VM; this is the substitute. Sampling skews towards instructions after slow ones.
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  pid_t pid = fork();
  if (!pid) { int o = open("/dev/null", O_WRONLY); dup2(o, 1); ptrace(PTRACE_TRACEME, 0, 0, 0); execv(argv[1], argv + 1); _exit(127); }
  int st; waitpid(pid, &st, 0);                      // stopped at exec
  enum { HB = 1 << 22 }; uint32_t *hist = calloc(HB, 4); uint64_t base = 0x400000, n = 0, other = 0;
  struct timespec ts = {0, 250000};
  ptrace(PTRACE_CONT, pid, 0, 0);
  for (;;) {
    nanosleep(&ts, 0);
    if (kill(pid, SIGSTOP)) break;                   // a traced process stops on any signal; this one is then discarded
    if (waitpid(pid, &st, 0) != pid || !WIFSTOPPED(st)) break;
    struct user_regs_struct r; if (ptrace(PTRACE_GETREGS, pid, 0, &r)) break;
    if (r.rip >= base && r.rip - base < HB) hist[r.rip - base]++; else other++;
    n++;
    if (ptrace(PTRACE_CONT, pid, 0, WSTOPSIG(st) == SIGSTOP ? 0 : WSTOPSIG(st))) break; }
  const char *out = getenv("SAMPLE_OUT"); FILE *f = fopen(out ? out : "sample.txt", "w");
  for (uint64_t i = 0; i < HB; i++) if (hist[i]) fprintf(f, "%llu 0 %u\n", (unsigned long long)(base + i), hist[i]);
  fclose(f); fprintf(stderr, "%llu samples (%llu outside the program: kernel or stub)\n", (unsigned long long)n, (unsigned long long)other);
  return 0; }
