// gen.c N_MiB text|csv > file : deterministic benchmark inputs.
//   text: words of mostly lowercase letters, with capitals, digits, punctuation and a few non-ASCII bytes
//   csv:  records of numbers, words and quoted fields (which contain commas, newlines and doubled quotes)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static uint32_t s = 12345;
static uint32_t rnd(void) { s = s * 1664525 + 1013904223; return s >> 8; }
int main(int c, char **v) { size_t n = (size_t)atoi(v[1]) << 20, done = 0; static uint8_t b[1 << 16];
  if (c > 2 && !strcmp(v[2], "csv")) {
    while (done < n) { size_t j = 0;
      while (j < sizeof b - 256) {
        for (int f = 0; f < 6; f++) { uint32_t r = rnd() % 10;
          if (r < 4) j += sprintf((char *)b + j, "%u", rnd() % 100000);
          else if (r < 8) { for (int k = 3 + rnd() % 8; k; k--) b[j++] = 97 + rnd() % 26; }
          else { b[j++] = '"'; for (int k = 5 + rnd() % 20; k; k--) { uint32_t q = rnd() % 40; if (q == 0) b[j++] = ','; else if (q == 1) b[j++] = '\n'; else if (q == 2) { b[j++] = '"'; b[j++] = '"'; } else b[j++] = 97 + q % 26; } b[j++] = '"'; }
          b[j++] = f == 5 ? '\n' : ','; } }
      if (j > n - done) j = n - done;
      fwrite(b, 1, j, stdout); done += j; }
    return 0; }
  for (size_t i = 0; i < n; i += sizeof b) {
    for (size_t j = 0; j < sizeof b; j++) { uint32_t r = rnd() & 255;
      b[j] = r < 40 ? 32 : r < 46 ? 10 : r < 226 ? 97 + r % 26 : r < 238 ? 65 + r % 26 : r < 246 ? 48 + r % 10 : r < 252 ? ".,;:!?'\"#-()"[rnd() % 12] : 128 + (rnd() & 127); }
    fwrite(b, 1, sizeof b, stdout); }
  return 0; }
