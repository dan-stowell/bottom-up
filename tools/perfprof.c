// perfprof.c -- where do a Strand binary's cycles go?  perfprof PROG < input      (stdout of PROG is discarded)
// Uses the CPU's own counters through perf_event_open (user mode only: perf_event_paranoid is 2 here):
//   * totals: cycles and instructions in user mode, over all threads of the program;
//   * a sample of the instruction pointer every PERIOD cycles (default 20000), on whichever thread and CPU
//     the cycles were spent, written as "address 0 count" lines to the file named by SAMPLE_OUT
//     (default sample.txt) for tools/whattime.py.
// This replaces tools/sample.c, which stopped the process on a timer: that under-counted nothing in user
// code but could not tell running from waiting, followed one thread, and cost a context switch per sample
// (the fifth audit measured how misleading that gets). Time in the kernel -- page faults, mostly -- is not
// in these counters at all: read it from `time`'s system time.
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
enum { PAGES = 64, HB = 1 << 22 };
static uint32_t *hist; static uint64_t base = 0x400000, nsamp, other, lost;
static int ev(struct perf_event_attr *a, pid_t pid, int cpu) { return syscall(SYS_perf_event_open, a, pid, cpu, -1, 0); }
static void drain(struct perf_event_mmap_page *m) {
  uint64_t head = __atomic_load_n(&m->data_head, __ATOMIC_ACQUIRE), tail = m->data_tail, size = PAGES * 4096ull;
  uint8_t *d = (uint8_t *)m + 4096;
  while (tail < head) {
    struct perf_event_header h; for (unsigned i = 0; i < sizeof h; i++) ((uint8_t *)&h)[i] = d[(tail + i) % size];
    if (h.type == PERF_RECORD_SAMPLE) {
      uint64_t ip = 0; for (int i = 0; i < 8; i++) ((uint8_t *)&ip)[i] = d[(tail + sizeof h + i) % size];
      nsamp++; if (ip >= base && ip - base < HB) hist[ip - base]++; else other++; }
    else if (h.type == PERF_RECORD_LOST) lost++;
    tail += h.size; }
  __atomic_store_n(&m->data_tail, tail, __ATOMIC_RELEASE); }
int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: perfprof PROG < input\n"); return 2; }
  int go[2]; if (pipe(go)) return 1;
  pid_t pid = fork();
  if (!pid) { char c; close(go[1]); if (read(go[0], &c, 1) != 1) _exit(126); int o = open("/dev/null", O_WRONLY); dup2(o, 1); execv(argv[1], argv + 1); _exit(127); }
  close(go[0]);
  hist = calloc(HB, 4);
  struct perf_event_attr a; memset(&a, 0, sizeof a);
  a.type = PERF_TYPE_HARDWARE; a.size = sizeof a; a.exclude_kernel = 1; a.exclude_hv = 1; a.inherit = 1; a.disabled = 1; a.enable_on_exec = 1;
  a.config = PERF_COUNT_HW_CPU_CYCLES; int fc = ev(&a, pid, -1);
  a.config = PERF_COUNT_HW_INSTRUCTIONS; int fi = ev(&a, pid, -1);
  if (fc < 0 || fi < 0) { perror("perf_event_open"); return 1; }
  // sampling: inherited events cannot share one ring across CPUs, so one event and ring per CPU
  int ncpu = sysconf(_SC_NPROCESSORS_ONLN), nf = 0; struct pollfd pf[256]; struct perf_event_mmap_page *ring[256];
  a.config = PERF_COUNT_HW_CPU_CYCLES; a.sample_period = getenv("PERIOD") ? strtoull(getenv("PERIOD"), 0, 0) : 20000; a.sample_type = PERF_SAMPLE_IP; a.wakeup_events = 256;
  for (int c = 0; c < ncpu && nf < 256; c++) {
    int f = ev(&a, pid, c); if (f < 0) continue;
    void *m = mmap(0, (1 + PAGES) * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0); if (m == MAP_FAILED) { close(f); continue; }
    pf[nf].fd = f; pf[nf].events = POLLIN; ring[nf++] = m; }
  if (write(go[1], "g", 1) != 1) return 1;
  int st = 0;
  for (;;) {
    poll(pf, nf, 20);
    for (int i = 0; i < nf; i++) drain(ring[i]);
    if (waitpid(pid, &st, WNOHANG) == pid) break; }
  for (int i = 0; i < nf; i++) drain(ring[i]);
  uint64_t cyc = 0, ins = 0; if (read(fc, &cyc, 8) != 8 || read(fi, &ins, 8) != 8) return 1;
  struct rusage ru; getrusage(RUSAGE_CHILDREN, &ru);
  const char *out = getenv("SAMPLE_OUT"); FILE *f = fopen(out ? out : "sample.txt", "w");
  for (uint64_t i = 0; i < HB; i++) if (hist[i]) fprintf(f, "%llu 0 %u\n", (unsigned long long)(base + i), hist[i]);
  fclose(f);
  fprintf(stderr, "user mode: %.1f M cycles, %.1f M instructions (%.2f per cycle); user %.0f ms, system %.0f ms; exit status %d\n",
          cyc / 1e6, ins / 1e6, cyc ? (double)ins / cyc : 0, ru.ru_utime.tv_sec * 1e3 + ru.ru_utime.tv_usec / 1e3, ru.ru_stime.tv_sec * 1e3 + ru.ru_stime.tv_usec / 1e3,
          WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st));
  fprintf(stderr, "%llu samples (%llu outside the program image, %llu lost records)\n", (unsigned long long)nsamp, (unsigned long long)other, (unsigned long long)lost);
  return 0; }
