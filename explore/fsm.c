// fsm.c -- is a small automaton a shape worth building? In-memory kernels, one thread.
// Task: strip sh-style comments: '#' to end of line is dropped, except inside "..." (with \ escapes).
// States: 0 code, 1 comment, 2 string, 3 string-after-backslash.
//   gcc -O3 -march=native -o fsm fsm.c && ./fsm FILE
#define _GNU_SOURCE
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

// 1. what one writes first: a switch
__attribute__((noinline)) static size_t strip_switch(const uint8_t *in, size_t n, uint8_t *out) {
  size_t k = 0; int s = 0;
  for (size_t i = 0; i < n; i++) { uint8_t c = in[i];
    switch (s) {
      case 0: if (c == '#') s = 1; else if (c == '"') s = 2; break;
      case 1: if (c == '\n') s = 0; break;
      case 2: if (c == '"') s = 0; else if (c == '\\') s = 3; break;
      case 3: s = 2; break; }
    if (s != 1) out[k++] = c; }
  return k; }
// 2. what one writes when it matters: a table, branch-free
static uint8_t T[4][256];
__attribute__((noinline)) static size_t strip_table(const uint8_t *in, size_t n, uint8_t *out) {
  size_t k = 0; unsigned s = 0;
  for (size_t i = 0; i < n; i++) { uint8_t c = in[i]; s = T[s][c]; out[k] = c; k += s != 1; }
  return k; }
// 3. the in-register scan of transition functions (the proposed stencil), 64 bytes per step
static uint8_t CLS[256]; static uint32_t MAP[16];
__attribute__((noinline)) static size_t strip_simd(const uint8_t *in, size_t n, uint8_t *out) {
  const __m512i OFFS = _mm512_set4_epi32(0x0c0c0c0c, 0x08080808, 0x04040404, 0), IDENT = _mm512_set1_epi32(0x03020100);
  __m512i t0 = _mm512_loadu_si512(CLS), t1 = _mm512_loadu_si512(CLS + 64), t2 = _mm512_loadu_si512(CLS + 128), t3 = _mm512_loadu_si512(CLS + 192);
  __m512i tmap = _mm512_loadu_si512(MAP), carry = IDENT; size_t k = 0, i = 0;
  for (; i + 64 <= n; i += 64) {
    __m512i c = _mm512_loadu_si512(in + i);
    __m512i lo = _mm512_permutex2var_epi8(t0, c, t1), hi = _mm512_permutex2var_epi8(t2, c, t3);
    __m512i cls = _mm512_mask_blend_epi8(_mm512_movepi8_mask(c), lo, hi);
    __m512i P[4];
    for (int j = 0; j < 4; j++) P[j] = _mm512_permutexvar_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(cls, j)), tmap);
    for (int s = 1; s < 16; s *= 2)
      for (int j = 3; j >= 0; j--) { __m512i p = j ? P[j - 1] : IDENT, l;
        switch (s) { case 1: l = _mm512_alignr_epi32(P[j], p, 15); break; case 2: l = _mm512_alignr_epi32(P[j], p, 14); break;
                     case 4: l = _mm512_alignr_epi32(P[j], p, 12); break; default: l = _mm512_alignr_epi32(P[j], p, 8); }
        P[j] = _mm512_shuffle_epi8(P[j], _mm512_or_si512(l, OFFS)); }
    for (int j = 3; j >= 1; j--) P[j] = _mm512_shuffle_epi8(P[j], _mm512_or_si512(P[j - 1], OFFS));
    for (int j = 3; j >= 2; j--) P[j] = _mm512_shuffle_epi8(P[j], _mm512_or_si512(P[j - 2], OFFS));
    __m512i co = _mm512_or_si512(carry, OFFS);
    for (int j = 0; j < 4; j++) P[j] = _mm512_shuffle_epi8(P[j], co);
    carry = _mm512_permutexvar_epi32(_mm512_set1_epi32(15), P[3]);
    __m512i st = _mm512_castsi128_si512(_mm512_cvtepi32_epi8(P[0]));
    st = _mm512_inserti32x4(st, _mm512_cvtepi32_epi8(P[1]), 1); st = _mm512_inserti32x4(st, _mm512_cvtepi32_epi8(P[2]), 2); st = _mm512_inserti32x4(st, _mm512_cvtepi32_epi8(P[3]), 3);
    __mmask64 keep = _mm512_cmpneq_epi8_mask(st, _mm512_set1_epi8(1));
    _mm512_storeu_si512(out + k, _mm512_maskz_compress_epi8(keep, c)); k += _mm_popcnt_u64(keep); }
  unsigned s = _mm_cvtsi128_si32(_mm512_castsi512_si128(carry)) & 3;
  for (; i < n; i++) { uint8_t c = in[i]; s = T[s][c]; out[k] = c; k += s != 1; }
  return k; }
__attribute__((noinline)) static size_t count_simd(const uint8_t *in, size_t n, uint8_t *out) {
  const __m512i OFFS = _mm512_set4_epi32(0x0c0c0c0c, 0x08080808, 0x04040404, 0), IDENT = _mm512_set1_epi32(0x03020100);
  __m512i t0 = _mm512_loadu_si512(CLS), t1 = _mm512_loadu_si512(CLS + 64), t2 = _mm512_loadu_si512(CLS + 128), t3 = _mm512_loadu_si512(CLS + 192);
  __m512i tmap = _mm512_loadu_si512(MAP), carry = IDENT; size_t k = 0, i = 0;
  for (; i + 64 <= n; i += 64) {
    __m512i c = _mm512_loadu_si512(in + i);
    __m512i lo = _mm512_permutex2var_epi8(t0, c, t1), hi = _mm512_permutex2var_epi8(t2, c, t3);
    __m512i cls = _mm512_mask_blend_epi8(_mm512_movepi8_mask(c), lo, hi);
    __m512i P[4];
    for (int j = 0; j < 4; j++) P[j] = _mm512_permutexvar_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(cls, j)), tmap);
    for (int s = 1; s < 16; s *= 2)
      for (int j = 3; j >= 0; j--) { __m512i p = j ? P[j - 1] : IDENT, l;
        switch (s) { case 1: l = _mm512_alignr_epi32(P[j], p, 15); break; case 2: l = _mm512_alignr_epi32(P[j], p, 14); break;
                     case 4: l = _mm512_alignr_epi32(P[j], p, 12); break; default: l = _mm512_alignr_epi32(P[j], p, 8); }
        P[j] = _mm512_shuffle_epi8(P[j], _mm512_or_si512(l, OFFS)); }
    for (int j = 3; j >= 1; j--) P[j] = _mm512_shuffle_epi8(P[j], _mm512_or_si512(P[j - 1], OFFS));
    for (int j = 3; j >= 2; j--) P[j] = _mm512_shuffle_epi8(P[j], _mm512_or_si512(P[j - 2], OFFS));
    __m512i co = _mm512_or_si512(carry, OFFS);
    for (int j = 0; j < 4; j++) P[j] = _mm512_shuffle_epi8(P[j], co);
    carry = _mm512_permutexvar_epi32(_mm512_set1_epi32(15), P[3]);
    __m512i st = _mm512_castsi128_si512(_mm512_cvtepi32_epi8(P[0]));
    st = _mm512_inserti32x4(st, _mm512_cvtepi32_epi8(P[1]), 1); st = _mm512_inserti32x4(st, _mm512_cvtepi32_epi8(P[2]), 2); st = _mm512_inserti32x4(st, _mm512_cvtepi32_epi8(P[3]), 3);
    __mmask64 keep = _mm512_cmpneq_epi8_mask(st, _mm512_set1_epi8(1));
    k += _mm_popcnt_u64(_mm512_cmpeq_epi8_mask(st, _mm512_setzero_si512())); }
  unsigned s = _mm_cvtsi128_si32(_mm512_castsi512_si128(carry)) & 3;
  for (; i < n; i++) { uint8_t c = in[i]; s = T[s][c]; k += s == 0; }
  return k; }
// 4. same automaton, states only, no filter: count bytes that are code (state 0)
__attribute__((noinline)) static size_t count_table(const uint8_t *in, size_t n, uint8_t *out) {
  size_t k = 0; unsigned s = 0; for (size_t i = 0; i < n; i++) { s = T[s][in[i]]; k += s == 0; } return k; }


// 3b. transposed layout: register t holds the maps of bytes 4g+t, one dword per group g.
// Level 1 is a 3-step chain across the four registers (all 16 groups at once), level 2 a
// Kogge-Stone scan over the 16 group maps in one register. ~36 zmm ops per 64 bytes.
#define FSM2_BODY(SINK) \
  const __m512i OFFS = _mm512_set4_epi32(0x0c0c0c0c, 0x08080808, 0x04040404, 0), IDO = _mm512_or_si512(_mm512_set1_epi32(0x03020100), OFFS); \
  const __m512i FOUR = _mm512_set1_epi8(4), EIGHT = _mm512_set1_epi8(8), REP = _mm512_set4_epi32(0x0c0c0c0c, 0x08080808, 0x04040404, 0), L15 = _mm512_set1_epi32(15); \
  const __m512i CFIX = _mm512_set4_epi32(0, 0x0c0c0c0c, 0x08080808, 0x04040404); \
  __m512i t0 = _mm512_loadu_si512(CLS), t1 = _mm512_loadu_si512(CLS + 64), t2 = _mm512_loadu_si512(CLS + 128), t3 = _mm512_loadu_si512(CLS + 192); \
  __m512i tmap = _mm512_loadu_si512(MAP), carry = _mm512_set1_epi32(0x0f0e0d0c); size_t k = 0, i = 0; \
  for (; i + 64 <= n; i += 64) { \
    __m512i c = _mm512_loadu_si512(in + i); \
    __m512i lo = _mm512_permutex2var_epi8(t0, c, t1), hi = _mm512_permutex2var_epi8(t2, c, t3); \
    __m512i cls = _mm512_mask_blend_epi8(_mm512_movepi8_mask(c), lo, hi); \
    __m512i A0 = _mm512_or_si512(_mm512_permutexvar_epi32(cls, tmap), OFFS); \
    __m512i A1 = _mm512_or_si512(_mm512_shuffle_epi8(_mm512_permutexvar_epi32(_mm512_srli_epi32(cls, 8), tmap), A0), OFFS); \
    __m512i A2 = _mm512_or_si512(_mm512_shuffle_epi8(_mm512_permutexvar_epi32(_mm512_srli_epi32(cls, 16), tmap), A1), OFFS); \
    __m512i G = _mm512_or_si512(_mm512_shuffle_epi8(_mm512_permutexvar_epi32(_mm512_srli_epi32(cls, 24), tmap), A2), OFFS), A3 = G; \
    G = _mm512_shuffle_epi8(G, _mm512_add_epi8(_mm512_alignr_epi32(G, IDO, 15), FOUR)); \
    G = _mm512_shuffle_epi8(G, _mm512_add_epi8(_mm512_alignr_epi32(G, IDO, 14), EIGHT)); \
    G = _mm512_shuffle_epi8(G, _mm512_alignr_epi32(G, IDO, 12)); \
    G = _mm512_shuffle_epi8(G, _mm512_alignr_epi32(G, IDO, 8)); \
    __m512i co = _mm512_add_epi8(carry, CFIX); \
    __m512i Y = _mm512_shuffle_epi8(G, co); \
    __m512i X = _mm512_add_epi8(_mm512_alignr_epi32(Y, co, 15), FOUR); \
    carry = _mm512_permutexvar_epi32(L15, Y); \
    __m512i XI = _mm512_shuffle_epi8(X, REP); \
    __m512i st = _mm512_maskz_shuffle_epi8(0x1111111111111111ull, A0, XI); \
    st = _mm512_mask_shuffle_epi8(st, 0x2222222222222222ull, A1, XI); \
    st = _mm512_mask_shuffle_epi8(st, 0x4444444444444444ull, A2, XI); \
    st = _mm512_mask_shuffle_epi8(st, 0x8888888888888888ull, A3, XI); \
    st = _mm512_and_si512(st, _mm512_set1_epi8(3)); \
    SINK } \
  unsigned s = _mm_cvtsi128_si32(_mm512_castsi512_si128(carry)) & 3;
__attribute__((noinline)) static size_t strip_simd2(const uint8_t *in, size_t n, uint8_t *out) {
  FSM2_BODY(__mmask64 keep = _mm512_cmpneq_epi8_mask(st, _mm512_set1_epi8(1)); _mm512_storeu_si512(out + k, _mm512_maskz_compress_epi8(keep, c)); k += _mm_popcnt_u64(keep);)
  for (; i < n; i++) { uint8_t c = in[i]; s = T[s][c]; out[k] = c; k += s != 1; }
  return k; }
__attribute__((noinline)) static size_t count_simd2(const uint8_t *in, size_t n, uint8_t *out) {
  FSM2_BODY(k += _mm_popcnt_u64(_mm512_cmpeq_epi8_mask(st, _mm512_setzero_si512()));)
  for (; i < n; i++) { s = T[s][in[i]]; k += s == 0; }
  return k; }
int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = ftell(f); rewind(f);
  uint8_t *in = malloc(n + 64), *o1 = malloc(n + 128), *o2 = malloc(n + 128); if (fread(in, 1, n, f) != n) return 1;
  for (int c = 0; c < 256; c++) { T[0][c] = c == '#' ? 1 : c == '"' ? 2 : 0; T[1][c] = c == '\n' ? 0 : 1; T[2][c] = c == '"' ? 0 : c == '\\' ? 3 : 2; T[3][c] = 2;
    CLS[c] = c == '#' ? 1 : c == '"' ? 2 : c == '\n' ? 3 : c == '\\' ? 4 : 0; }
  for (int c = 0; c < 256; c++) MAP[CLS[c]] = T[0][c] | T[1][c] << 8 | T[2][c] << 16 | T[3][c] << 24;
  memset(o1, 0, n); memset(o2, 0, n);
  struct { const char *name; size_t (*fn)(const uint8_t *, size_t, uint8_t *); } K[] = {
    {"C switch, gcc -O3", strip_switch}, {"C table, gcc -O3", strip_table}, {"in-register function scan", strip_simd}, {"(count only) C table", count_table},{"(count only) function scan", count_simd},{"function scan, transposed", strip_simd2},{"(count only) transposed", count_simd2}};
  size_t ref = strip_switch(in, n, o1);
  for (int k = 0; k < 7; k++) { double best = 1e9; size_t r = 0;
    for (int rep = 0; rep < 7; rep++) { double t = now(); r = K[k].fn(in, n, o2); t = now() - t; if (t < best) best = t; }
    int ok = (k >= 3 && k != 5) || (r == ref && !memcmp(o1, o2, ref));
    printf("%-30s %6.2f GB/s  %s (%zu)\n", K[k].name, n / best / 1e9, ok ? "ok" : "MISMATCH", r); }
  return 0; }
