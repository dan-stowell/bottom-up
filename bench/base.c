// base.c -- the obvious C version of each benchmark program, as a whole process:
// read stdin in 64 KiB blocks, write results the way the Strand version does.
// Built twice per program: gcc -O2 -fno-tree-vectorize ("scalar") and gcc -O3 -march=native ("autovec").
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "xlat.h"
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
#define P_strip 18
#define P_csv 19
#define P_idents 20
#define P_strings 21
#define P_xlat 22
#define P_json 23
static uint8_t ISW[256];
int main(void) {
  for (int c = 0; c < 256; c++) ISW[c] = (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_';
  uint32_t acc = 0; int32_t mx = INT32_MIN; int prev = 0; int32_t prev4 = 0; uint32_t run = 0; ssize_t n; size_t have = 0;
#if K == P_words
  prev = 0;
#endif
  while ((n = read(0, in + have, BLK - have)) > 0) {
    n += have; size_t n4 = n / 4; int32_t *w = (int32_t *)in; int32_t *o4 = (int32_t *)out; size_t no = 0;
#if K == P_lines || K == P_lines32
    for (ssize_t i = 0; i < n; i++) acc += in[i] == 10;
#elif K == P_words
    acc += (in[0] > 32) & !prev;                        // adjacent loads, not a carried flag: GCC vectorises this
    for (ssize_t i = 1; i < n; i++) acc += (in[i] > 32) & (in[i - 1] <= 32);
    prev = in[n - 1] > 32;
#elif K == P_sum
    for (ssize_t i = 0; i < n; i++) acc += in[i];
#elif K == P_pairs
    acc += (in[0] == 104) & (prev == 116);
    for (ssize_t i = 1; i < n; i++) acc += (in[i] == 104) & (in[i - 1] == 116);
    prev = in[n - 1];
#elif K == P_nonascii
    for (ssize_t i = 0; i < n; i++) acc += in[i] > 127;
#elif K == P_upper
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; out[i] = c - (((c >= 97) & (c <= 122)) << 5); }
    no = n;
#elif K == P_rot13
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; int m = (c >= 97) & (c <= 122); int x = c - 97 + 13; x -= (x >= 26) * 26; out[i] = m ? x + 97 : c; }
    no = n;
#elif K == P_swapcase
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; int u = c & 223; out[i] = c ^ (((u >= 65) & (u <= 90)) * 32); }   // arithmetic, not a table: this vectorises (second audit)
    no = n;
#elif K == P_xlat
    for (ssize_t i = 0; i < n; i++) out[i] = XLAT[in[i]];
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
#elif K == P_strip
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i];
      switch (prev) { case 0: if (c == '#') prev = 1; else if (c == '"') prev = 2; break; case 1: if (c == '\n') prev = 0; break;
                      case 2: if (c == '"') prev = 0; else if (c == '\\') prev = 3; break; default: prev = 2; }
      out[no] = c; no += prev != 1; }
#elif K == P_idents       // words of [A-Za-z0-9_] that begin in code (not in a comment or a string)
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i];
      switch (prev) { case 0: if (c == '#') prev = 1; else if (c == '"') prev = 2; break; case 1: if (c == '\n') prev = 0; break;
                      case 2: if (c == '"') prev = 0; else if (c == '\\') prev = 3; break; default: prev = 2; }
      int wc = ISW[c];
      acc += wc & !prev4 & (prev == 0); prev4 = wc; }
#elif K == P_strings      // string literals: entries into the string state from code
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; int was = prev;
      switch (prev) { case 0: if (c == '#') prev = 1; else if (c == '"') prev = 2; break; case 1: if (c == '\n') prev = 0; break;
                      case 2: if (c == '"') prev = 0; else if (c == '\\') prev = 3; break; default: prev = 2; }
      acc += (prev == 2) & (was == 0); }
#elif K == P_json         // the structural characters of JSON text: { } [ ] : , outside strings
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i];
      switch (prev) { case 0: if (c == '"') prev = 1; break; case 1: if (c == '"') prev = 0; else if (c == '\\') prev = 2; break; default: prev = 1; }
      out[no] = c; no += (prev == 0) & ((c == '{') | (c == '}') | (c == '[') | (c == ']') | (c == ':') | (c == ',')); }
#elif K == P_csv
    for (ssize_t i = 0; i < n; i++) { uint8_t c = in[i]; prev ^= c == '"'; acc += ((c == ',') | (c == '\n')) & !prev; }
#endif
    if (no) put(out, no);
#if K >= P_sum4 && K <= P_prefix4      // dword kernels only keep a partial trailing dword (AUDIT.md, defect 2)
    have = n - n4 * 4; memmove(in, in + n4 * 4, have);
#endif
  }
#if K == P_maxline || K == P_maxgap4
  put(&mx, 4);
#elif K != P_swapcase && K != P_xlat && K != P_json && K != P_upper && K != P_rot13 && K != P_letters && K != P_scale4 && K != P_filter4 && K != P_prefix4 && K != P_strip
  put(&acc, 4);
#endif
  return 0;
}
