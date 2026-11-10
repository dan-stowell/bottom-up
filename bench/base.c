// base.c -- the obvious C version of each benchmark program, as a whole process:
// read stdin in 64 KiB blocks, write results the way the Strand version does.
// Built twice per program: gcc -O2 -fno-tree-vectorize ("scalar") and gcc -O3 -march=native ("autovec").
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#define BLK (1 << 16)
static uint8_t in[BLK]; static uint8_t out[BLK * 4 + 64];
static void put(const void *p, size_t n) { const char *c = p; while (n) { ssize_t w = write(1, c, n); if (w <= 0) _exit(1); c += w; n -= w; } }
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
int main(void) {
  uint32_t acc = 0; int32_t mx = INT32_MIN; int prev = 0; int32_t prev4 = 0; uint32_t run = 0; ssize_t n; size_t have = 0;
#if K == P_words
  prev = 0;
#endif
  while ((n = read(0, in + have, BLK - have)) > 0) {
    n += have; size_t n4 = n / 4; int32_t *w = (int32_t *)in; int32_t *o4 = (int32_t *)out; size_t no = 0;
#if K == P_lines || K == P_lines32
    for (ssize_t i = 0; i < n; i++) acc += in[i] == 10;
#elif K == P_words
    for (ssize_t i = 0; i < n; i++) { int nb = in[i] > 32; acc += nb & !prev; prev = nb; }
#elif K == P_sum
    for (ssize_t i = 0; i < n; i++) acc += in[i];
#elif K == P_pairs
    for (ssize_t i = 0; i < n; i++) { acc += (in[i] == 104) & (prev == 116); prev = in[i]; }
#elif K == P_nonascii
    for (ssize_t i = 0; i < n; i++) acc += in[i] > 127;
#elif K == P_upper
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; out[i] = c - (((c >= 97) & (c <= 122)) << 5); }
    no = n;
#elif K == P_rot13
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; int m = (c >= 97) & (c <= 122); int x = c - 97 + 13; x -= (x >= 26) * 26; out[i] = m ? x + 97 : c; }
    no = n;
#elif K == P_swapcase
    { static uint8_t tab[256]; if (!tab[1]) for (int i = 0; i < 256; i++) { int u = i & 223; tab[i] = i ^ (((u >= 65) & (u <= 90)) * 32); }
      for (ssize_t i = 0; i < n; i++) out[i] = tab[in[i]]; }
    no = n;
#elif K == P_maxline
    for (ssize_t i = 0; i < n; i++) { if (in[i] == 10) run = 0; else { run++; if ((int32_t)run > mx) mx = run; } }
    if (mx < 0) mx = 0;
#elif K == P_letters
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; out[no] = c; no += (c >= 97) & (c <= 122); }
#elif K == P_sum4
    for (size_t i = 0; i < n4; i++) acc += w[i];
#elif K == P_sumpos4
    for (size_t i = 0; i < n4; i++) acc += w[i] > 0 ? w[i] : 0;
#elif K == P_maxgap4
    for (size_t i = 0; i < n4; i++) { int32_t d = (int32_t)((uint32_t)w[i] - (uint32_t)prev4); if (d > mx) mx = d; prev4 = w[i]; }
#elif K == P_scale4
    for (size_t i = 0; i < n4; i++) o4[i] = (int32_t)((uint32_t)w[i] * 3u + 7u);
    no = n4 * 4;
#elif K == P_filter4
    { size_t k = 0; for (size_t i = 0; i < n4; i++) { o4[k] = w[i]; k += w[i] > 0; } no = k * 4; }
#elif K == P_prefix4
    for (size_t i = 0; i < n4; i++) { acc += w[i]; o4[i] = acc; }
    no = n4 * 4;
#endif
    if (no) put(out, no);
#if K >= P_sum4
    have = n - n4 * 4; memmove(in, in + n4 * 4, have);
#endif
  }
#if K == P_maxline || K == P_maxgap4
  put(&mx, 4);
#elif K != P_swapcase && K != P_upper && K != P_rot13 && K != P_letters && K != P_scale4 && K != P_filter4 && K != P_prefix4
  put(&acc, 4);
#endif
  return 0;
}
