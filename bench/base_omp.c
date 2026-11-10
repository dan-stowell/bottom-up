// base_omp.c -- the strongest C I know how to write for each benchmark without intrinsics: mmap the
// input, gcc -O3 -march=native -fopenmp, loops shaped so that GCC vectorises them where it can
// (adjacent loads instead of a carried `prev`, AUDIT.md), and hand-written threading where a pragma
// is not enough (filters, prefix sums, longest line, automata). -DK selects the kernel.
// Output conventions match the Strand programs in programs.txt.
#define _GNU_SOURCE
#include <fcntl.h>
#include <omp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "xlat.h"
// (macros, not an enum: they are compared by the preprocessor)
#define P_lines 1
#define P_words 2
#define P_sum 3
#define P_pairs 4
#define P_nonascii 5
#define P_upper 6
#define P_rot13 7
#define P_maxline 8
#define P_letters 9
#define P_sum4 10
#define P_sumpos4 11
#define P_maxgap4 12
#define P_scale4 13
#define P_filter4 14
#define P_prefix4 15
#define P_swapcase 16
#define P_lines32 17
#define P_strip 18
#define P_csv 19
#define P_idents 20
#define P_strings 21
#define P_xlat 22
#define P_json 23
static void put(const void *p, size_t n) { const char *c = p; while (n) { ssize_t w = write(1, c, n); if (w <= 0) _exit(1); c += w; n -= w; } }
// Split [0,m) among the threads that actually run (not the number asked for).
#define RANGE int T = omp_get_num_threads(), t = omp_get_thread_num(); size_t per = m / T, lo = t * per, hi = t == T - 1 ? m : lo + per
enum { BLK = 4 << 20 };
#define K_strip_step(s, c) switch (s) { case 0: if (c == '#') s = 1; else if (c == '"') s = 2; break; case 1: if (c == '\n') s = 0; break; \
                                        case 2: if (c == '"') s = 0; else if (c == '\\') s = 3; break; default: s = 2; }
static uint8_t ISW[256];
int main(void) {
  for (int c = 0; c < 256; c++) ISW[c] = (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_';
  struct stat st; if (fstat(0, &st)) return 1; size_t n = st.st_size;
  const uint8_t *in = n ? mmap(0, n, PROT_READ, MAP_PRIVATE, 0, 0) : (const uint8_t *)""; if (in == MAP_FAILED) return 1;
  size_t n4 = n / 4; const int32_t *w = (const int32_t *)in; (void)n4; (void)w;
  int MT = omp_get_max_threads(); (void)MT;
  uint32_t acc = 0;
#if K == P_lines || K == P_lines32
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 0; i < n; i++) acc += in[i] == 10;
  put(&acc, 4);
#elif K == P_words
  acc = n && in[0] > 32;
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 1; i < n; i++) acc += (in[i] > 32) & (in[i - 1] <= 32);
  put(&acc, 4);
#elif K == P_sum
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 0; i < n; i++) acc += in[i];
  put(&acc, 4);
#elif K == P_pairs
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 1; i < n; i++) acc += (in[i] == 104) & (in[i - 1] == 116);
  put(&acc, 4);
#elif K == P_nonascii
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 0; i < n; i++) acc += in[i] > 127;
  put(&acc, 4);
#elif K == P_sum4
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 0; i < n4; i++) acc += w[i];
  put(&acc, 4);
#elif K == P_sumpos4
  #pragma omp parallel for reduction(+:acc) schedule(static)
  for (size_t i = 0; i < n4; i++) acc += w[i] > 0 ? w[i] : 0;
  put(&acc, 4);
#elif K == P_maxgap4
  int32_t mx = n4 ? w[0] : INT32_MIN;
  #pragma omp parallel for reduction(max:mx) schedule(static)
  for (size_t i = 1; i < n4; i++) { int32_t d = (int32_t)((uint32_t)w[i] - (uint32_t)w[i - 1]); if (d > mx) mx = d; }
  put(&mx, 4);
#elif K == P_maxline
  // per range: bytes before the first newline, longest run after it, bytes after the last newline
  size_t *lead = calloc(MT, sizeof *lead), *inner = calloc(MT, sizeof *inner), *tail = calloc(MT, sizeof *tail); int *has = calloc(MT, sizeof *has), TT = 1;
  size_t m = n;
  #pragma omp parallel
  { RANGE; size_t run = 0, best = 0, ld = 0; int seen = 0;
    #pragma omp single
    TT = T;
    for (size_t i = lo; i < hi; i++) { if (in[i] == 10) { if (!seen) { ld = run; seen = 1; } run = 0; } else { run++; if (seen && run > best) best = run; } }
    if (!seen) ld = run;
    lead[t] = ld; inner[t] = best; tail[t] = run; has[t] = seen; }
  size_t cur = 0, best = 0;
  for (int t = 0; t < TT; t++) { if (!has[t]) { cur += lead[t]; if (cur > best) best = cur; }
    else { if (cur + lead[t] > best) best = cur + lead[t]; if (inner[t] > best) best = inner[t]; cur = tail[t]; if (cur > best) best = cur; } }
  int32_t mx = (int32_t)best; put(&mx, 4);
#elif K == P_upper || K == P_rot13 || K == P_swapcase || K == P_xlat
  uint8_t *out = malloc(BLK);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < m; i++) { uint8_t c = in[b + i];
  #if K == P_upper
      out[i] = c - (((c >= 97) & (c <= 122)) << 5);
  #elif K == P_rot13
      int lower = (c >= 97) & (c <= 122); int x = c - 97 + 13; x -= (x >= 26) * 26; out[i] = lower ? x + 97 : c;
  #elif K == P_swapcase
      int u = c & 223; out[i] = c ^ (((u >= 65) & (u <= 90)) * 32);       // arithmetic, not a table: this vectorises (second audit)
  #else
      out[i] = XLAT[c];
  #endif
    }
    put(out, m); }
#elif K == P_scale4
  uint32_t *out = malloc(BLK);
  for (size_t b = 0; b < n4; b += BLK / 4) { size_t m = n4 - b < BLK / 4 ? n4 - b : BLK / 4;
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < m; i++) out[i] = (uint32_t)w[b + i] * 3u + 7u;
    put(out, m * 4); }
#elif K == P_letters
  size_t *cnt = calloc(MT, sizeof *cnt); uint8_t **reg = malloc(MT * sizeof *reg); int TT = 1;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel
    { RANGE; size_t k = 0; uint8_t *o = reg[t];
      #pragma omp single
      TT = T;
      for (size_t i = lo; i < hi; i++) { uint8_t c = in[b + i]; o[k] = c; k += (c >= 97) & (c <= 122); }
      cnt[t] = k; }
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t]); }
#elif K == P_filter4
  size_t *cnt = calloc(MT, sizeof *cnt); int32_t **reg = malloc(MT * sizeof *reg); int TT = 1;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n4; b += BLK / 4) { size_t m = n4 - b < BLK / 4 ? n4 - b : BLK / 4;
    #pragma omp parallel
    { RANGE; size_t k = 0; int32_t *o = reg[t];
      #pragma omp single
      TT = T;
      for (size_t i = lo; i < hi; i++) { o[k] = w[b + i]; k += w[b + i] > 0; }
      cnt[t] = k; }
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t] * 4); }
#elif K == P_prefix4
  // two-pass parallel prefix sum: per-thread totals, then offsets
  uint32_t *out = malloc(BLK), carry = 0, *tot = calloc(MT + 1, 4); int TT = 1;
  for (size_t b = 0; b < n4; b += BLK / 4) { size_t m = n4 - b < BLK / 4 ? n4 - b : BLK / 4;
    #pragma omp parallel
    { RANGE; uint32_t s = 0;
      #pragma omp single
      TT = T;
      for (size_t i = lo; i < hi; i++) s += w[b + i];
      tot[t + 1] = s;
      #pragma omp barrier
      #pragma omp single
      { tot[0] = carry; for (int u = 0; u < T; u++) tot[u + 1] += tot[u]; }
      s = tot[t];
      for (size_t i = lo; i < hi; i++) { s += w[b + i]; out[i] = s; } }
    carry = tot[TT]; put(out, m * 4); }
#elif K == P_strip
  // an automaton in parallel, by hand: pass 1 runs every range from all four states at once to learn
  // its transition function; the start states follow; pass 2 runs each range for real.
  size_t *cnt = calloc(MT, sizeof *cnt); uint8_t **reg = malloc(MT * sizeof *reg), (*fn)[4] = calloc(MT, 4), *start = calloc(MT + 1, 1); int TT = 1, s0 = 0;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel
    { RANGE; uint8_t *o = reg[t]; size_t k = 0;
      #pragma omp single
      TT = T;
      if (T > 1) { int a = 0, b1 = 1, c2 = 2, d = 3;
        for (size_t i = lo; i < hi; i++) { uint8_t c = in[b + i]; K_strip_step(a, c) K_strip_step(b1, c) K_strip_step(c2, c) K_strip_step(d, c) }
        fn[t][0] = a; fn[t][1] = b1; fn[t][2] = c2; fn[t][3] = d; }
      #pragma omp barrier
      #pragma omp single
      { start[0] = s0; for (int u = 0; u < T; u++) start[u + 1] = T > 1 ? fn[u][start[u]] : 0; }
      int s = start[t];
      for (size_t i = lo; i < hi; i++) { uint8_t c = in[b + i]; K_strip_step(s, c) o[k] = c; k += s != 1; }
      cnt[t] = k; if (t == T - 1) start[T] = s; }
    s0 = start[TT];
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t]); }
#elif K == P_json
  // the same two passes as strip for the three states of JSON text: 0 outside, 1 in a string, 2 after a backslash
  #define J_step(s, c) switch (s) { case 0: if (c == '"') s = 1; break; case 1: if (c == '"') s = 0; else if (c == '\\') s = 2; break; default: s = 1; }
  size_t *cnt = calloc(MT, sizeof *cnt); uint8_t **reg = malloc(MT * sizeof *reg), (*fn)[4] = calloc(MT, 4), *start = calloc(MT + 1, 1); int TT = 1, s0 = 0;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel
    { RANGE; uint8_t *o = reg[t]; size_t k = 0;
      #pragma omp single
      TT = T;
      if (T > 1) { int a = 0, b1 = 1, c2 = 2;
        for (size_t i = lo; i < hi; i++) { uint8_t c = in[b + i]; J_step(a, c) J_step(b1, c) J_step(c2, c) }
        fn[t][0] = a; fn[t][1] = b1; fn[t][2] = c2; }
      #pragma omp barrier
      #pragma omp single
      { start[0] = s0; for (int u = 0; u < T; u++) start[u + 1] = T > 1 ? fn[u][start[u]] : 0; }
      int s = start[t];
      for (size_t i = lo; i < hi; i++) { uint8_t c = in[b + i]; J_step(s, c) o[k] = c; k += (s == 0) & ((c == '{') | (c == '}') | (c == '[') | (c == ']') | (c == ':') | (c == ',')); }
      cnt[t] = k; if (t == T - 1) start[T] = s; }
    s0 = start[TT];
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t]); }
#elif K == P_idents || K == P_strings
  // the same two passes as strip, counting instead of copying
  uint8_t (*fn)[4] = calloc(MT, 4), *start = calloc(MT + 1, 1); size_t m = n;
  #pragma omp parallel reduction(+:acc)
  { RANGE;
    if (T > 1) { int a = 0, b1 = 1, c2 = 2, d = 3;
      for (size_t i = lo; i < hi; i++) { uint8_t c = in[i]; K_strip_step(a, c) K_strip_step(b1, c) K_strip_step(c2, c) K_strip_step(d, c) }
      fn[t][0] = a; fn[t][1] = b1; fn[t][2] = c2; fn[t][3] = d; }
    #pragma omp barrier
    #pragma omp single
    { start[0] = 0; for (int u = 0; u < T; u++) start[u + 1] = T > 1 ? fn[u][start[u]] : 0; }
    int s = start[t];
  #if K == P_idents
    int pw = 0; if (lo) { uint8_t c = in[lo - 1]; pw = ISW[c]; }
    for (size_t i = lo; i < hi; i++) { uint8_t c = in[i]; K_strip_step(s, c)
      int wc = ISW[c];
      acc += wc & !pw & (s == 0); pw = wc; }
  #else
    for (size_t i = lo; i < hi; i++) { uint8_t c = in[i]; int was = s; K_strip_step(s, c) acc += (s == 2) & (was == 0); }
  #endif
  }
  put(&acc, 4);
#elif K == P_csv
  // separators (comma, newline) outside quotes. Each range counts for both possible quote states at
  // its start and reports whether it flips the state.
  uint32_t (*c2)[2] = calloc(MT, 8); int *flip = calloc(MT, sizeof *flip), TT = 1; size_t m = n;
  #pragma omp parallel
  { RANGE; uint32_t a0 = 0, a1 = 0; unsigned q = 0;
    #pragma omp single
    TT = T;
    for (size_t i = lo; i < hi; i++) { uint8_t c = in[i]; q ^= c == '"'; unsigned sep = (c == ',') | (c == '\n'); a0 += sep & ~q; a1 += sep & q; }
    c2[t][0] = a0; c2[t][1] = a1; flip[t] = q; }
  { unsigned q = 0; for (int t = 0; t < TT; t++) { acc += c2[t][q]; q ^= flip[t]; } }
  put(&acc, 4);
#endif
  return 0;
}
