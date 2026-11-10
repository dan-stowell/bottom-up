// link.c -- what shape does linking have, and which parts of it have to be that shape?
// A synthetic link job with the proportions of a large C++ program, each part done several ways.
//   U units; every unit defines D symbols, refers to R others by name, and has code with relocations
//   sorted by offset. S = U*D global symbols, mangled-looking names of 24..88 bytes.
// Parts:
//   join    undefined reference -> definition.   (a) by name: hash the string, probe a table, compare.
//           (b) by 64-bit fingerprint the compiler already computed.  (c) the same as a SIMD scatter
//           race (what Strand's own name resolution does).  (d) by (unit, ordinal): no table at all.
//   layout  address of every unit and symbol: prefix sums.
//   reloc   (a) copy the code, then for each relocation read the symbol's address and write 4 bytes.
//           (b) streaming: gather the values, then one pass that blends them into the code through a
//           mask of hole bytes (vpexpandb). No scatter.
//   gcc -O3 -march=native -o link link.c && ./link
#define _GNU_SOURCE
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec*1e-6; }
static uint64_t rs = 88172645463325252ull;
static inline uint64_t rnd(void){ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static inline uint64_t mix(uint64_t x){ x ^= x >> 32; x *= 0xd6e8feb86659fd93ull; x ^= x >> 32; x *= 0xd6e8feb86659fd93ull; return x ^ (x >> 32); }
static inline uint64_t hash_str(const char *s, int n){ uint64_t h = n * 0x9e3779b97f4a7c15ull, w;      // 8 bytes at a time
  while (n >= 8) { memcpy(&w, s, 8); h = mix(h ^ w); s += 8; n -= 8; }
  if (n) { w = 0; memcpy(&w, s, n); h = mix(h ^ w); } return h; }
enum { U = 2000, D = 500, R = 1500, S = U * D, RELOCS = 6000, CODE = 128 << 10 };
int main(void) {
  // ---- the job
  char *names = malloc((size_t)S * 96); uint32_t *noff = malloc(4 * (S + 1)); size_t np = 0;
  for (int i = 0; i < S; i++) { noff[i] = np; int n = 24 + rnd() % 65; np += sprintf(names + np, "_ZN%d", i);
    while ((int)(np - noff[i]) < n) names[np++] = 'a' + rnd() % 26; }
  noff[S] = np;
  uint32_t *undef = malloc(4ull * U * R);                       // unit u refers to these global symbols (by name, to the linker)
  for (size_t i = 0; i < (size_t)U * R; i++) { uint64_t r = rnd(); undef[i] = (r % 8 ? r >> 8 : (r >> 8) % 4096 * 37) % S; }   // some symbols are popular
  uint32_t *roff = malloc(4ull * U * RELOCS), *rsym = malloc(4ull * U * RELOCS);   // per unit: offset in its code, index into its undef list
  for (int u = 0; u < U; u++) { uint32_t o = 0; for (int k = 0; k < RELOCS; k++) { o += 8 + rnd() % 27; if (o + 4 > CODE) o = CODE - 4; roff[(size_t)u * RELOCS + k] = o; rsym[(size_t)u * RELOCS + k] = rnd() % R; } }
  uint8_t *code = malloc((size_t)U * CODE + 64), *out = malloc((size_t)U * CODE + 64), *out2 = malloc((size_t)U * CODE + 64);
  for (size_t i = 0; i < (size_t)U * CODE; i += 8) { uint64_t r = rnd(); memcpy(code + i, &r, 8); }
  memset(out, 1, (size_t)U * CODE); memset(out2, 1, (size_t)U * CODE);
  printf("%d units, %d symbols (%.0f MB of names), %d references by name, %d relocations, %d MB of code\n\n", U, S, np / 1e6, U * R, U * RELOCS, (int)((size_t)U * CODE >> 20));
  uint32_t *res = malloc(4ull * U * R), *res2 = malloc(4ull * U * R); double t;
  // every unit carries its own copy of the names it refers to, in order (as an object file's string table does)
  size_t usz = 0; for (size_t i = 0; i < (size_t)U * R; i++) usz += noff[undef[i] + 1] - noff[undef[i]];
  char *unames = malloc(usz + 8); uint32_t *ulen = malloc(4ull * U * R); usz = 0;
  for (size_t i = 0; i < (size_t)U * R; i++) { int g = undef[i]; ulen[i] = noff[g + 1] - noff[g]; memcpy(unames + usz, names + noff[g], ulen[i]); usz += ulen[i]; }
  printf("(reference strings: %.0f MB, read in order; definition strings are compared at random)\n", usz / 1e6);

  // ---- join (a): by name
  size_t M = 1; while (M < 2 * (size_t)S) M *= 2; int32_t *tab = malloc(4 * M); uint64_t *th = malloc(8 * M);
  t = now(); memset(tab, -1, 4 * M);
  for (int i = 0; i < S; i++) { uint64_t h = hash_str(names + noff[i], noff[i + 1] - noff[i]); size_t k = h & (M - 1); while (tab[k] >= 0) k = (k + 1) & (M - 1); tab[k] = i; th[k] = h; }
  const char *s = unames;
  for (size_t i = 0; i < (size_t)U * R; i++, s += ulen[i - 1]) { int n = ulen[i];
    uint64_t h = hash_str(s, n); size_t k = h & (M - 1);
    while (!(th[k] == h && noff[tab[k] + 1] - noff[tab[k]] == (uint32_t)n && !memcmp(names + noff[tab[k]], s, n))) k = (k + 1) & (M - 1);
    res[i] = tab[k]; }
  double ja = now() - t; printf("join by name (hash string, probe, compare)        %7.1f ms   %5.1f ns per name\n", ja, ja * 1e6 / (S + (double)U * R));
  t = now(); { uint64_t x = 0; const char *q = unames; for (size_t i = 0; i < (size_t)U * R; i++) { x ^= hash_str(q, ulen[i]); q += ulen[i] ; } for (int i = 0; i < S; i++) x ^= hash_str(names + noff[i], noff[i + 1] - noff[i]);
    double th_ = now() - t; printf("  of which: fingerprinting every name, in order     %7.1f ms   %5.1f ns per name   (%.1f GB/s; checksum %llx)\n", th_, th_ * 1e6 / (S + (double)U * R), (usz + np) / th_ / 1e6, (unsigned long long)x); }
  // ---- join (b): by fingerprint computed at compile time
  uint64_t *fp = malloc(8 * S), *ufp = malloc(8ull * U * R);
  for (int i = 0; i < S; i++) fp[i] = hash_str(names + noff[i], noff[i + 1] - noff[i]);
  for (size_t i = 0; i < (size_t)U * R; i++) ufp[i] = fp[undef[i]];
  t = now(); memset(tab, -1, 4 * M);
  for (int i = 0; i < S; i++) { size_t k = fp[i] & (M - 1); while (tab[k] >= 0) k = (k + 1) & (M - 1); tab[k] = i; th[k] = fp[i]; }
  for (size_t i = 0; i < (size_t)U * R; i++) { size_t k = ufp[i] & (M - 1); while (th[k] != ufp[i]) k = (k + 1) & (M - 1); res2[i] = tab[k]; }
  double jb = now() - t; printf("join by 64-bit fingerprint (probe only)             %7.1f ms   %5.1f ns per name   %s\n", jb, jb * 1e6 / (S + (double)U * R), memcmp(res, res2, 4ull * U * R) ? "MISMATCH" : "same result");
  // ---- join (c): fingerprints, as a SIMD scatter race: 4 rounds of scatter / gather / compare, 8 qword lanes
  int32_t *tab4 = malloc(16 * M); int32_t *tabr[4] = {tab4, tab4 + M, tab4 + 2 * M, tab4 + 3 * M};      // one table per round
  t = now(); memset(tab4, -1, 16 * M); memset(res2, 255, 4ull * U * R);
  { uint8_t *placed = calloc(S, 1); size_t left = S; const uint64_t K[4] = {1, 0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull};
    for (int r = 0; r < 4 && left; r++) {            // definitions claim a slot; the last writer owns it; losers go again
      for (int i = 0; i < S; i++) if (!placed[i]) tabr[r][(fp[i] * K[r] >> 20) & (M - 1)] = i;      // (scalar here; vpscatterdd does the same)
      for (int i = 0; i < S; i++) if (!placed[i] && tabr[r][(fp[i] * K[r] >> 20) & (M - 1)] == i) { placed[i] = r + 1; left--; } }
    // references: gather the slot of each round, keep the one whose fingerprint matches
    const __m512i mask = _mm512_set1_epi64(M - 1);
    for (size_t i = 0; i + 8 <= (size_t)U * R; i += 8) { __m512i f = _mm512_loadu_si512(ufp + i); __m256i got = _mm256_set1_epi32(-1); __mmask8 need = 0xff;
      for (int r = 0; r < 4 && need; r++) { __m512i k = _mm512_and_si512(_mm512_srli_epi64(_mm512_mullo_epi64(f, _mm512_set1_epi64(K[r])), 20), mask);
        __m256i c = _mm512_mask_i64gather_epi32(_mm256_set1_epi32(-1), need, k, tabr[r], 4);
        __mmask8 ok = need & _mm256_cmpgt_epi32_mask(c, _mm256_set1_epi32(-1));
        __m512i cf = _mm512_mask_i32gather_epi64(_mm512_setzero_si512(), ok, c, fp, 8);
        ok &= _mm512_cmpeq_epi64_mask(cf, f); got = _mm256_mask_mov_epi32(got, ok, c); need &= ~ok; }
      _mm256_storeu_si256((void *)(res2 + i), got); }
    double jc = now() - t; size_t bad = 0; for (size_t i = 0; i + 8 <= (size_t)U * R; i += 8) for (int j = 0; j < 8; j++) bad += res2[i + j] != res[i + j];
    printf("join by fingerprint, scatter race + SIMD gathers    %7.1f ms   %5.1f ns per name   %zu definitions lost all 4 rounds, %zu references unresolved\n", jc, jc * 1e6 / (S + (double)U * R), left, bad); }
  // ---- join (d): the reference is (unit, ordinal): nothing to do at link time
  printf("join by (unit, ordinal)                             %7.1f ms   (the compiler resolved the name when it read the interface)\n\n", 0.0);

  // ---- layout: prefix sums over units, then symbol addresses
  uint64_t *ubase = malloc(8 * (U + 1)), *addr = malloc(8 * S);
  t = now(); ubase[0] = 0x400000; for (int u = 0; u < U; u++) ubase[u + 1] = ubase[u] + CODE;
  for (int i = 0; i < S; i++) addr[i] = ubase[i / D] + (uint64_t)(i % D) * (CODE / D);
  printf("layout (prefix sums, one address per symbol)        %7.1f ms\n\n", now() - t);

  // ---- imports: every unit gets the addresses of the symbols it refers to (one random read per reference)
  uint64_t *imp = malloc(8ull * U * R);
  t = now(); for (size_t i = 0; i < (size_t)U * R; i++) imp[i] = addr[res[i]];
  printf("import addresses (one read per reference)           %7.1f ms   %5.1f ns per reference\n\n", now() - t, (now() - t) * 1e6 / ((double)U * R));
  // ---- relocation (a): copy, then patch each site
  t = now(); memcpy(out, code, (size_t)U * CODE); double tc = now() - t;
  for (int u = 0; u < U; u++) { uint8_t *o = out + (size_t)u * CODE; const uint32_t *ro = roff + (size_t)u * RELOCS, *rsy = rsym + (size_t)u * RELOCS; const uint64_t *im = imp + (size_t)u * R;
    for (int k = 0; k < RELOCS; k++) { uint32_t v = (uint32_t)(im[rsy[k]] - (ubase[u] + ro[k])); memcpy(o + ro[k], &v, 4); } }
  double ra = now() - t; printf("relocation: copy, then patch each site              %7.1f ms   (%.1f ms of it the copy; %.1f ns per relocation)\n", ra, tc, (ra - tc) * 1e6 / ((double)U * RELOCS));
  // ---- relocation (b): values by gather, then one streaming pass that expands them into the holes
  uint64_t *holes = calloc(((size_t)U * CODE >> 6) + 1, 8); uint32_t *vals = malloc(4ull * U * RELOCS + 64); size_t nv = 0;
  for (int u = 0; u < U; u++) { uint32_t last = ~0u; for (int k = 0; k < RELOCS; k++) { uint32_t o = roff[(size_t)u * RELOCS + k]; if (o == last) continue; last = o;      // (the compiler would emit this mask)
      size_t p = (size_t)u * CODE + o; for (int b = 0; b < 4; b++) holes[(p + b) >> 6] |= 1ull << ((p + b) & 63); } }
  t = now();
  for (int u = 0; u < U; u++) { const uint32_t *ro = roff + (size_t)u * RELOCS, *rsy = rsym + (size_t)u * RELOCS; const uint64_t *im = imp + (size_t)u * R; uint32_t last = ~0u; int k = 0;
    const __m512i place0 = _mm512_set1_epi64(ubase[u]);
    for (; k + 8 <= RELOCS; k += 8) {                 // 8 relocations per step: gather the import's address, subtract the place
      __m512i a = _mm512_i32gather_epi64(_mm256_loadu_si256((void *)(rsy + k)), im, 8);
      __m512i pl = _mm512_add_epi64(place0, _mm512_cvtepu32_epi64(_mm256_loadu_si256((void *)(ro + k))));
      __m256i v = _mm512_cvtepi64_epi32(_mm512_sub_epi64(a, pl));
      uint32_t tmp[8]; _mm256_storeu_si256((void *)tmp, v);
      for (int j = 0; j < 8; j++) { if (ro[k + j] != last) vals[nv++] = tmp[j]; else vals[nv - 1] = tmp[j]; last = ro[k + j]; } }      // duplicates at the clamp: last wins
    for (; k < RELOCS; k++) { uint32_t v = (uint32_t)(im[rsy[k]] - (ubase[u] + ro[k])); if (ro[k] != last) vals[nv++] = v; else vals[nv - 1] = v; last = ro[k]; } }
  double tg = now() - t; const uint8_t *vp = (const uint8_t *)vals;
  for (size_t i = 0; i < (size_t)U * CODE; i += 64) { __mmask64 m = holes[i >> 6];
    _mm512_storeu_si512(out2 + i, _mm512_mask_expandloadu_epi8(_mm512_loadu_si512(code + i), m, vp)); vp += _mm_popcnt_u64(m); }
  double rb = now() - t; printf("relocation: gather values, expand into the holes    %7.1f ms   (%.1f ms of it the gathers)   %s\n", rb, tg, memcmp(out, out2, (size_t)U * CODE) ? "MISMATCH" : "same bytes");
  return 0; }
