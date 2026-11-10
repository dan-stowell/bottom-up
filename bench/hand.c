// hand.c -- what an expert writes by hand for the programs where auto-vectorised C loses: AVX-512
// intrinsics, mmap, OpenMP, the same techniques the Strand stencils use (compress for filters, byte
// permutes for tables, a scan of transition functions for automata) and, where a better algorithm for
// the task is known, that algorithm instead (csv: carry-less multiply for quote parity, as simdjson
// does). This is the comparison the second audit asked for: it separates "Strand generates this code
// from one line" from "this code is fast". -DK selects the kernel; output conventions as programs.txt.
//   gcc -O3 -march=native -fopenmp -DK=P_strip -o hand_strip hand.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <immintrin.h>
#include <omp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "xlat.h"
#define P_letters 9
#define P_filter4 14
#define P_strip 18
#define P_csv 19
#define P_idents 20
#define P_strings 21
#define P_xlat 22
#define P_json 23
static void put(const void *p, size_t n) { const char *c = p; while (n) { ssize_t w = write(1, c, n); if (w <= 0) _exit(1); c += w; n -= w; } }
// Split [0,m) among the threads that actually run; every range but the last is a multiple of 64.
#define RANGE int T = omp_get_num_threads(), t = omp_get_thread_num(); size_t per = (m / T) & ~(size_t)63, lo = t * per, hi = t == T - 1 ? m : lo + per
enum { BLK = 4 << 20 };

// ---- the 4-state automaton of strip / idents / strings: 0 code, 1 comment, 2 string, 3 after backslash
static uint8_t TR[4][256], CLS[256]; static uint32_t MAP[16];
static void fsm_init(void) {
  for (int c = 0; c < 256; c++) { TR[0][c] = c == '#' ? 1 : c == '"' ? 2 : 0; TR[1][c] = c == '\n' ? 0 : 1; TR[2][c] = c == '"' ? 0 : c == '\\' ? 3 : 2; TR[3][c] = 2;
    CLS[c] = c == '#' ? 1 : c == '"' ? 2 : c == '\n' ? 3 : c == '\\' ? 4 : 0; }
  for (int c = 0; c < 256; c++) MAP[CLS[c]] = TR[0][c] | TR[1][c] << 8 | TR[2][c] << 16 | TR[3][c] << 24; }
// One 64-byte step. Register t holds the functions of bytes 4g+t, one dword per group g; a 3-step
// chain composes within groups, a Kogge-Stone scan composes the 16 group functions, the carry (the
// function so far, pre-offset by 12) comes last. Returns the state after each byte if STATES.
#define FSM_CONSTS \
  const __m512i OFFS = _mm512_set4_epi32(0x0c0c0c0c, 0x08080808, 0x04040404, 0), IDO = _mm512_or_si512(_mm512_set1_epi32(0x03020100), OFFS); \
  const __m512i FOUR = _mm512_set1_epi8(4), EIGHT = _mm512_set1_epi8(8), L15 = _mm512_set1_epi32(15); \
  const __m512i CFIX = _mm512_set4_epi32(0, 0x0c0c0c0c, 0x08080808, 0x04040404); \
  const __m512i t0 = _mm512_loadu_si512(CLS), t1 = _mm512_loadu_si512(CLS + 64), t2 = _mm512_loadu_si512(CLS + 128), t3 = _mm512_loadu_si512(CLS + 192), tmap = _mm512_loadu_si512(MAP);
#define FSM_STEP(c, carry, st, STATES) { \
    __m512i lo_ = _mm512_permutex2var_epi8(t0, c, t1), hi_ = _mm512_permutex2var_epi8(t2, c, t3); \
    __m512i cls = _mm512_mask_blend_epi8(_mm512_movepi8_mask(c), lo_, hi_); \
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
    carry = _mm512_permutexvar_epi32(L15, Y); \
    if (STATES) { \
      __m512i X = _mm512_add_epi8(_mm512_alignr_epi32(Y, co, 15), FOUR); \
      __m512i XI = _mm512_shuffle_epi8(X, OFFS); \
      st = _mm512_maskz_shuffle_epi8(0x1111111111111111ull, A0, XI); \
      st = _mm512_mask_shuffle_epi8(st, 0x2222222222222222ull, A1, XI); \
      st = _mm512_mask_shuffle_epi8(st, 0x4444444444444444ull, A2, XI); \
      st = _mm512_mask_shuffle_epi8(st, 0x8888888888888888ull, A3, XI); \
      st = _mm512_and_si512(st, _mm512_set1_epi8(3)); } }
// the transition function of in[0..n): f[s] = state after the range when entered in state s
__attribute__((noinline)) static void fsm_fn(const uint8_t *in, size_t n, uint8_t f[4]) {
  FSM_CONSTS; __m512i carry = _mm512_set1_epi32(0x0f0e0d0c), st; size_t i = 0; (void)st;
  for (; i + 64 <= n; i += 64) { __m512i c = _mm512_loadu_si512(in + i); FSM_STEP(c, carry, st, 0) }
  uint32_t w = _mm_cvtsi128_si32(_mm512_castsi512_si128(carry));
  for (int s = 0; s < 4; s++) { unsigned q = (w >> (8 * s)) & 3; for (size_t j = i; j < n; j++) q = TR[q][in[j]]; f[s] = q; } }
#define ENTER(s) _mm512_set1_epi8(12 | (s))       // the carry that puts the automaton in state s

// ---- JSON, the way simdjson's first stage does it: no automaton. Escaped characters by an add-carry
// trick on the backslash mask, string interiors by a carry-less multiply (prefix xor) of the quote mask.
static inline uint64_t escaped(uint64_t bs, uint64_t *prev) {
  bs &= ~*prev; uint64_t follows = bs << 1 | *prev; const uint64_t even = 0x5555555555555555ull;
  uint64_t odd_starts = bs & ~even & ~follows, seq;
  *prev = __builtin_add_overflow(odd_starts, bs, (unsigned long long *)&seq);
  return (even ^ (seq << 1)) & follows; }
#define J_MASKS(c) \
    uint64_t quo = _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8('"')) & ~escaped(_mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8('\\')), &esc); \
    uint64_t ins = _mm_cvtsi128_si64(_mm_clmulepi64_si128(_mm_cvtsi64_si128(quo), _mm_set1_epi8(-1), 0)) ^ inq; inq = (uint64_t)((int64_t)ins >> 63);
#define J_SCALAR(s, c) switch (s) { case 0: if (c == '"') s = 1; break; case 1: if (c == '"') s = 0; else if (c == '\\') s = 2; break; default: s = 1; }

int main(void) {
  struct stat sb; if (fstat(0, &sb)) return 1; size_t n = sb.st_size;
  const uint8_t *in = n ? mmap(0, n, PROT_READ, MAP_PRIVATE, 0, 0) : (const uint8_t *)""; if (in == MAP_FAILED) return 1;
  size_t n4 = n / 4; const int32_t *w = (const int32_t *)in; (void)n4; (void)w;
  int MT = omp_get_max_threads(); (void)MT;
  uint32_t acc = 0; (void)acc;
#if K == P_xlat
  uint8_t *out = malloc(BLK);
  const __m512i x0 = _mm512_loadu_si512(XLAT), x1 = _mm512_loadu_si512(XLAT + 64), x2 = _mm512_loadu_si512(XLAT + 128), x3 = _mm512_loadu_si512(XLAT + 192);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK, v = m & ~(size_t)63;
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < v; i += 64) { __m512i c = _mm512_loadu_si512(in + b + i);
      __m512i lo = _mm512_permutex2var_epi8(x0, c, x1), hi = _mm512_permutex2var_epi8(x2, c, x3);
      _mm512_storeu_si512(out + i, _mm512_mask_blend_epi8(_mm512_movepi8_mask(c), lo, hi)); }
    for (size_t i = v; i < m; i++) out[i] = XLAT[in[b + i]];
    put(out, m); }
#elif K == P_letters
  size_t *cnt = calloc(MT, sizeof *cnt); uint8_t **reg = malloc(MT * sizeof *reg); int TT = 1;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel
    { RANGE; size_t k = 0, i = lo; uint8_t *o = reg[t];
      #pragma omp single
      TT = T;
      for (; i + 64 <= hi; i += 64) { __m512i c = _mm512_loadu_si512(in + b + i);
        __mmask64 keep = _mm512_cmplt_epu8_mask(_mm512_sub_epi8(c, _mm512_set1_epi8(97)), _mm512_set1_epi8(26));
        _mm512_storeu_si512(o + k, _mm512_maskz_compress_epi8(keep, c)); k += _mm_popcnt_u64(keep); }
      for (; i < hi; i++) { uint8_t c = in[b + i]; o[k] = c; k += (c >= 97) & (c <= 122); }
      cnt[t] = k; }
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t]); }
#elif K == P_filter4
  size_t *cnt = calloc(MT, sizeof *cnt); int32_t **reg = malloc(MT * sizeof *reg); int TT = 1;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n4; b += BLK / 4) { size_t m = n4 - b < BLK / 4 ? n4 - b : BLK / 4;
    #pragma omp parallel
    { RANGE; size_t k = 0, i = lo; int32_t *o = reg[t];
      #pragma omp single
      TT = T;
      for (; i + 16 <= hi; i += 16) { __m512i x = _mm512_loadu_si512(w + b + i);
        __mmask16 keep = _mm512_cmpgt_epi32_mask(x, _mm512_setzero_si512());
        _mm512_storeu_si512(o + k, _mm512_maskz_compress_epi32(keep, x)); k += _mm_popcnt_u32(keep); }
      for (; i < hi; i++) { o[k] = w[b + i]; k += w[b + i] > 0; }
      cnt[t] = k; }
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t] * 4); }
#elif K == P_strip
  fsm_init();
  size_t *cnt = calloc(MT, sizeof *cnt); uint8_t **reg = malloc(MT * sizeof *reg), (*fn)[4] = calloc(MT, 4), *start = calloc(MT + 1, 1); int TT = 1, s0 = 0;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel
    { RANGE; uint8_t *o = reg[t]; size_t k = 0, i = lo;
      #pragma omp single
      TT = T;
      if (T > 1) fsm_fn(in + b + lo, hi - lo, fn[t]);
      #pragma omp barrier
      #pragma omp single
      { start[0] = s0; for (int u = 0; u < T; u++) start[u + 1] = T > 1 ? fn[u][start[u]] : 0; }
      FSM_CONSTS; __m512i carry = ENTER(start[t]), st = carry;
      for (; i + 64 <= hi; i += 64) { __m512i c = _mm512_loadu_si512(in + b + i); FSM_STEP(c, carry, st, 1)
        __mmask64 keep = _mm512_cmpneq_epi8_mask(st, _mm512_set1_epi8(1));
        _mm512_storeu_si512(o + k, _mm512_maskz_compress_epi8(keep, c)); k += _mm_popcnt_u64(keep); }
      unsigned s = _mm_cvtsi128_si32(_mm512_castsi512_si128(carry)) & 3;
      for (; i < hi; i++) { uint8_t c = in[b + i]; s = TR[s][c]; o[k] = c; k += s != 1; }
      cnt[t] = k; if (t == T - 1) start[T] = s; }
    s0 = start[TT];
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t]); }
#elif K == P_idents || K == P_strings
  fsm_init();
  uint8_t (*fn)[4] = calloc(MT, 4), *start = calloc(MT + 1, 1); size_t m = n;
  #pragma omp parallel reduction(+:acc)
  { RANGE; size_t i = lo;
    if (T > 1) fsm_fn(in + lo, hi - lo, fn[t]);
    #pragma omp barrier
    #pragma omp single
    { start[0] = 0; for (int u = 0; u < T; u++) start[u + 1] = T > 1 ? fn[u][start[u]] : 0; }
    FSM_CONSTS; unsigned s = start[t]; __m512i carry = ENTER(s), st = carry;
  #if K == P_idents
    uint64_t pw = lo ? ((in[lo - 1] | 32) - 97u < 26) | (in[lo - 1] - 48u < 10) | (in[lo - 1] == 95) : 0;
    for (; i + 64 <= hi; i += 64) { __m512i c = _mm512_loadu_si512(in + i); FSM_STEP(c, carry, st, 1)
      __mmask64 wd = _mm512_cmplt_epu8_mask(_mm512_sub_epi8(_mm512_or_si512(c, _mm512_set1_epi8(32)), _mm512_set1_epi8(97)), _mm512_set1_epi8(26))
                   | _mm512_cmplt_epu8_mask(_mm512_sub_epi8(c, _mm512_set1_epi8(48)), _mm512_set1_epi8(10)) | _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8(95));
      acc += _mm_popcnt_u64(wd & ~(wd << 1 | pw) & _mm512_cmpeq_epi8_mask(st, _mm512_setzero_si512())); pw = wd >> 63; }
    s = _mm_cvtsi128_si32(_mm512_castsi512_si128(carry)) & 3;
    for (; i < hi; i++) { uint8_t c = in[i]; s = TR[s][c]; unsigned wc = ((c | 32) - 97u < 26) | (c - 48u < 10) | (c == 95); acc += wc & !pw & (s == 0); pw = wc; }
  #elif K == P_strings
    uint64_t p0 = s == 0;
    for (; i + 64 <= hi; i += 64) { __m512i c = _mm512_loadu_si512(in + i); FSM_STEP(c, carry, st, 1)
      __mmask64 z = _mm512_cmpeq_epi8_mask(st, _mm512_setzero_si512()), two = _mm512_cmpeq_epi8_mask(st, _mm512_set1_epi8(2));
      acc += _mm_popcnt_u64(two & (z << 1 | p0)); p0 = z >> 63; }
    s = _mm_cvtsi128_si32(_mm512_castsi512_si128(carry)) & 3;
    for (; i < hi; i++) { unsigned was = s; s = TR[s][in[i]]; acc += (s == 2) & (was == 0); }
  #endif
  }
  put(&acc, 4);
#elif K == P_json
  // Pass 1: does the range flip "inside a string"? (Whether it starts escaped is read off the backslashes
  // just before it.) Pass 2: compress the structural characters outside strings.
  size_t *cnt = calloc(MT, sizeof *cnt); uint8_t **reg = malloc(MT * sizeof *reg), *flip = calloc(MT, 1), *start = calloc(MT + 1, 1); int TT = 1, s0 = 0, e0 = 0;
  for (int t = 0; t < MT; t++) reg[t] = malloc(BLK + 64);
  for (size_t b = 0; b < n; b += BLK) { size_t m = n - b < BLK ? n - b : BLK;
    #pragma omp parallel
    { RANGE; uint8_t *o = reg[t]; size_t k = 0, i = lo; uint64_t esc0 = e0, esc, inq = 0;
      #pragma omp single
      TT = T;
      if (t) { size_t j = lo; while (j > 0 && in[b + j - 1] == '\\') j--; esc0 = (lo - j) & 1; if (j == 0 && e0) esc0 ^= 1; }
      if (T > 1) { esc = esc0; size_t j = lo;
        for (; j + 64 <= hi; j += 64) { __m512i c = _mm512_loadu_si512(in + b + j); J_MASKS(c) }
        unsigned q = inq & 1, e = esc;     // the tail, still without knowing whether the range began inside a string
        for (; j < hi; j++) { uint8_t c = in[b + j]; if (e) e = 0; else if (c == '\\') e = 1; else q ^= c == '"'; }
        flip[t] = q; }
      #pragma omp barrier
      #pragma omp single
      { start[0] = s0; for (int u = 0; u < T; u++) start[u + 1] = T > 1 ? start[u] ^ flip[u] : 0; }
      esc = esc0; inq = start[t] ? ~0ull : 0;
      for (; i + 64 <= hi; i += 64) { __m512i c = _mm512_loadu_si512(in + b + i); J_MASKS(c)
        __mmask64 st = _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8('{')) | _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8('}')) | _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8('['))
                     | _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8(']')) | _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8(':')) | _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8(','));
        __mmask64 keep = st & ~ins;
        _mm512_storeu_si512(o + k, _mm512_maskz_compress_epi8(keep, c)); k += _mm_popcnt_u64(keep); }
      unsigned s = esc ? 2 : (inq & 1);
      for (; i < hi; i++) { uint8_t c = in[b + i]; J_SCALAR(s, c) o[k] = c; k += (s == 0) & ((c == '{') | (c == '}') | (c == '[') | (c == ']') | (c == ':') | (c == ',')); }
      cnt[t] = k; if (t == T - 1) { start[T] = s != 0; e0 = s == 2; } }
    s0 = start[TT];
    for (int t = 0; t < TT; t++) put(reg[t], cnt[t]); }
#elif K == P_csv
  // quote parity by carry-less multiplication: no automaton at all
  uint32_t (*c2)[2] = calloc(MT, 8); int *flip = calloc(MT, sizeof *flip), TT = 1; size_t m = n;
  #pragma omp parallel
  { RANGE; uint32_t a0 = 0, a1 = 0; uint64_t q = 0; size_t i = lo;
    #pragma omp single
    TT = T;
    for (; i + 64 <= hi; i += 64) { __m512i c = _mm512_loadu_si512(in + i);
      uint64_t quo = _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8('"')), sep = _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8(',')) | _mm512_cmpeq_epi8_mask(c, _mm512_set1_epi8('\n'));
      uint64_t inq = _mm_cvtsi128_si64(_mm_clmulepi64_si128(_mm_cvtsi64_si128(quo), _mm_set1_epi8(-1), 0)) ^ q;   // parity after each byte
      a0 += _mm_popcnt_u64(sep & ~inq); a1 += _mm_popcnt_u64(sep & inq); q = (uint64_t)((int64_t)inq >> 63); }
    unsigned qq = q & 1;
    for (; i < hi; i++) { uint8_t c = in[i]; qq ^= c == '"'; unsigned sp = (c == ',') | (c == '\n'); a0 += sp & ~qq; a1 += sp & qq; }
    c2[t][0] = a0; c2[t][1] = a1; flip[t] = qq; }
  { unsigned q = 0; for (int t = 0; t < TT; t++) { acc += c2[t][q]; q ^= flip[t]; } }
  put(&acc, 4);
#endif
  return 0;
}
