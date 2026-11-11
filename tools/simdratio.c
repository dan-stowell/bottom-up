// simdratio.c -- what does a Strand binary actually execute?  simdratio STUBLEN PROG < input
// Single-steps PROG under ptrace (one thread: run it under `taskset -c N`) and classifies every
// executed instruction by its encoding:
//   vector   EVEX or VEX instructions on zmm/ymm/xmm registers, other than the next two kinds
//   gather   vpgather* / vpscatter*: vector instructions that this CPU executes at scalar-loop prices
//   mask     k-register instructions (kmov, kand, kshift, kortest, ...)
//   scalar   everything else: general-register instructions, including VEX-coded BMI (bzhi, shlx, ...)
// separately for the runtime stub (the first STUBLEN bytes after the 112-byte ELF header) and for the
// code the compiler generated. Output is written to stderr; the program's stdout goes to /dev/null.
// Counts are instructions, not time and not elements: a vector instruction is one whether it moves
// 16 dwords or 64 bytes. (Where the time goes is tools/perfprof.c's business.) This is the exact answer to
// "how much of the run is the scalar glue". About a microsecond per instruction.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
enum { VEC, GATHER, MASK, SCALAR, NK };
static int classify(const uint8_t *p) {
  int i = 0;
  while (i < 8 && (p[i] == 0x66 || p[i] == 0x67 || p[i] == 0xf2 || p[i] == 0xf3 || p[i] == 0x2e || p[i] == 0x3e || p[i] == 0x26 || p[i] == 0x36 || p[i] == 0x64 || p[i] == 0x65 || p[i] == 0xf0)) i++;
  if (p[i] == 0x62) {                                   // EVEX: 62 P0 P1 P2 opcode
    int map = p[i + 1] & 7, op = p[i + 4];
    if (map == 2 && ((op >= 0x90 && op <= 0x93) || (op >= 0xa0 && op <= 0xa3))) return GATHER;
    return VEC; }
  if (p[i] == 0xc5 || p[i] == 0xc4) {                   // VEX
    int map = p[i] == 0xc5 ? 1 : p[i + 1] & 31, op = p[i] == 0xc5 ? p[i + 2] : p[i + 3];
    if (map == 1 && ((op >= 0x41 && op <= 0x4b) || (op >= 0x90 && op <= 0x93) || op == 0x98 || op == 0x99)) return MASK;
    if (map == 3 && (op >= 0x30 && op <= 0x33)) return MASK;                 // kshift
    if (map == 2 && (op == 0xf2 || op == 0xf3 || op == 0xf5 || op == 0xf6 || op == 0xf7)) return SCALAR;   // andn, bls*, bzhi, pext, pdep, mulx, shlx, sarx, shrx, bextr
    if (map == 3 && op == 0xf0) return SCALAR;                                // rorx
    if (map == 2 && (op >= 0x90 && op <= 0x93)) return GATHER;                // AVX2 gathers
    return VEC; }
  return SCALAR; }
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: simdratio STUBLEN PROG < input\n"); return 2; }
  uint64_t stub_lo = 0x400000 + 112, stub_hi = stub_lo + strtoull(argv[1], 0, 0);
  pid_t pid = fork();
  if (!pid) { int o = open("/dev/null", O_WRONLY); dup2(o, 1); ptrace(PTRACE_TRACEME, 0, 0, 0); execv(argv[2], argv + 2); _exit(127); }
  int st; waitpid(pid, &st, 0);
  uint64_t n[2][NK] = {{0}}, syscalls = 0;
  // SIMDRATIO_HIST=file: also write, for every generated-code address executed, "address kind count" lines
  const char *hf = getenv("SIMDRATIO_HIST"); enum { HB = 1 << 22 }; uint32_t (*hist)[NK] = hf ? calloc(HB, sizeof *hist) : 0; uint64_t hbase = stub_hi;
  while (WIFSTOPPED(st)) {
    struct user_regs_struct r; ptrace(PTRACE_GETREGS, pid, 0, &r);
    uint64_t w[2]; w[0] = ptrace(PTRACE_PEEKTEXT, pid, (void *)r.rip, 0); w[1] = ptrace(PTRACE_PEEKTEXT, pid, (void *)(r.rip + 8), 0);
    const uint8_t *p = (const uint8_t *)w;
    if (p[0] == 0x0f && p[1] == 0x05) syscalls++;
    int kd = classify(p); n[r.rip >= stub_lo && r.rip < stub_hi ? 0 : 1][kd]++;
    if (hist && r.rip >= hbase && r.rip - hbase < HB) hist[r.rip - hbase][kd]++;
    if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0)) break;
    waitpid(pid, &st, 0); }
  if (hist) { FILE *f = fopen(hf, "w"); for (uint64_t i = 0; i < HB; i++) for (int k = 0; k < NK; k++) if (hist[i][k]) fprintf(f, "%llu %d %u\n", (unsigned long long)(hbase + i), k, hist[i][k]); fclose(f); }
  static const char *K[NK] = {"vector", "gather/scatter", "mask", "scalar"};
  uint64_t tot = 0, all[NK] = {0}; for (int a = 0; a < 2; a++) for (int k = 0; k < NK; k++) { tot += n[a][k]; all[k] += n[a][k]; }
  fprintf(stderr, "%-16s %14s %14s %14s %8s\n", "", "stub", "generated", "all", "share");
  for (int k = 0; k < NK; k++) fprintf(stderr, "%-16s %14llu %14llu %14llu %7.1f%%\n", K[k], (unsigned long long)n[0][k], (unsigned long long)n[1][k], (unsigned long long)all[k], 100.0 * all[k] / (tot ? tot : 1));
  fprintf(stderr, "%-16s %14llu %14llu %14llu   (%llu system calls; exit status %d)\n", "total", (unsigned long long)(n[0][0] + n[0][1] + n[0][2] + n[0][3]), (unsigned long long)(n[1][0] + n[1][1] + n[1][2] + n[1][3]),
          (unsigned long long)tot, (unsigned long long)syscalls, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return 0; }
