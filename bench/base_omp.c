// base_omp.c -- the strongest C baseline I know how to write for these kernels: mmap the input,
// gcc -O3 -march=native -fopenmp, one parallel loop. -DK selects the kernel (see base.c for the
// plain versions). Output conventions match the Strand programs.
#define _GNU_SOURCE
#include <fcntl.h>
#include <omp.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define P_lines 1
#define P_words 2
#define P_upper 6
#define P_letters 9
#define P_prefix4 15
static void put(const void *p, size_t n) { const char *c = p; while (n) { ssize_t w = write(1, c, n); if (w <= 0) _exit(1); c += w; n -= w; } }
int main(void) {
  struct stat st; fstat(0, &st); size_t n = st.st_size;
  const uint8_t *in = n ? mmap(0, n, PROT_READ, MAP_PRIVATE, 0, 0) : (const uint8_t *)"";
  uint32_t acc = 0;
#if K == P_lines
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 0; i < n; i++) acc += in[i] == 10;
  put(&acc, 4);
#elif K == P_words
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 0; i < n; i++) acc += (in[i] > 32) & (i == 0 || in[i - 1] <= 32);
  put(&acc, 4);
#elif K == P_upper
  enum { BLK = 4 << 20 }; uint8_t *out = malloc(BLK);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < m; i++) { uint8_t c = in[b + i]; out[i] = c - (((c >= 97) & (c <= 122)) << 5); }
    put(out, m); }
#elif K == P_letters
  enum { BLK = 4 << 20 }; uint8_t *out = malloc(BLK); int T = 1;
  T = omp_get_max_threads();
  size_t *cnt = calloc(T + 1, sizeof *cnt); uint8_t **reg = malloc(T * sizeof *reg);
  for (int t = 0; t < T; t++) reg[t] = malloc(BLK / T + 64);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK, per = m / T;
    #pragma omp parallel num_threads(T)
    { int t = omp_get_thread_num(); size_t lo = t * per, hi = t == T - 1 ? m : lo + per, k = 0; uint8_t *o = reg[t];
      for (size_t i = lo; i < hi; i++) { uint8_t c = in[b + i]; o[k] = c; k += (c >= 97) & (c <= 122); }
      cnt[t] = k; }
    for (int t = 0; t < T; t++) put(reg[t], cnt[t]); }
#elif K == P_prefix4
  // two-pass parallel prefix sum: per-thread totals, then offsets
  size_t n4 = n / 4; const int32_t *w = (const int32_t *)in; enum { BLK = 1 << 20 }; uint32_t *out = malloc(BLK * 4); uint32_t carry = 0; int T = 1;
  T = omp_get_max_threads();
  uint32_t *tot = calloc(T + 1, 4);
  for (size_t b = 0; b < n4; b += BLK) { size_t m = n4 - b < BLK ? n4 - b : BLK, per = m / T;
    #pragma omp parallel num_threads(T)
    { int t = omp_get_thread_num(); size_t lo = t * per, hi = t == T - 1 ? m : lo + per; uint32_t s = 0;
      for (size_t i = lo; i < hi; i++) s += w[b + i];
      tot[t + 1] = s; }
    tot[0] = carry; for (int t = 0; t < T; t++) tot[t + 1] += tot[t];
    #pragma omp parallel num_threads(T)
    { int t = omp_get_thread_num(); size_t lo = t * per, hi = t == T - 1 ? m : lo + per; uint32_t s = tot[t];
      for (size_t i = lo; i < hi; i++) { s += w[b + i]; out[i] = s; } }
    carry = tot[T]; put(out, m * 4); }
#endif
  return 0;
}
