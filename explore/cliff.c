// cliff.c -- how much does the *spelling* of a loop decide whether GCC vectorises it?
// Five ways to count words (a non-blank byte whose predecessor is blank), all correct, all "obvious".
//   gcc -O3 -march=native -fopt-info-vec-optimized -o cliff cliff.c && ./cliff FILE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
#define NI __attribute__((noinline)) static uint32_t
NI w_flag(const uint8_t *in, size_t n) {      // 1. carry a flag
  uint32_t acc = 0; int prev = 0;
  for (size_t i = 0; i < n; i++) { int nb = in[i] > 32; acc += nb & !prev; prev = nb; }
  return acc; }
NI w_if(const uint8_t *in, size_t n) {        // 2. the textbook state machine
  uint32_t acc = 0; int inword = 0;
  for (size_t i = 0; i < n; i++) { if (in[i] <= 32) inword = 0; else if (!inword) { inword = 1; acc++; } }
  return acc; }
NI w_guard(const uint8_t *in, size_t n) {     // 3. look back, guard the first element in the loop
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i++) acc += (in[i] > 32) & (i == 0 || in[i - 1] <= 32);
  return acc; }
NI w_peel(const uint8_t *in, size_t n) {      // 4. look back, first element peeled off
  uint32_t acc = n && in[0] > 32;
  for (size_t i = 1; i < n; i++) acc += (in[i] > 32) & (in[i - 1] <= 32);
  return acc; }
NI w_prevbyte(const uint8_t *in, size_t n) {  // 5. carry the previous byte
  uint32_t acc = 0; uint8_t p = 0;
  for (size_t i = 0; i < n; i++) { acc += (in[i] > 32) & (p <= 32); p = in[i]; }
  return acc; }
int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = ftell(f); rewind(f); if (n > 64 << 20) n = 64 << 20;
  uint8_t *in = malloc(n); if (fread(in, 1, n, f) != n) return 1;
  struct { const char *name; uint32_t (*fn)(const uint8_t *, size_t); } K[] = {{"carry a flag", w_flag}, {"if/else state machine", w_if},
    {"look back, guard i == 0", w_guard}, {"look back, peeled", w_peel}, {"carry the previous byte", w_prevbyte}};
  for (int k = 0; k < 5; k++) { double best = 1e9; uint32_t r = 0;
    for (int rep = 0; rep < 7; rep++) { double t = now(); r = K[k].fn(in, n); t = now() - t; if (t < best) best = t; }
    printf("%-26s %6.2f GB/s  (%u)\n", K[k].name, n / best / 1e9, r); }
  return 0; }
