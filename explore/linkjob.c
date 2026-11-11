// linkjob.c -- a link job as a file, and a plain C linker for it (the reference for examples/link.strand).
//   linkjob gen U > job      units with 64..192 KiB of code each, 500 exports, 1,500 imports, ~1 relocation per 21 bytes
//   linkjob ref < job > image
// File: dwords, then code.
//   U NE NI NR NC | usize[U] uexp[U] | exunit[NE] exoff[NE] efp1[NE] efp2[NE] | imu[NI] imo[NI] ifp1[NI] ifp2[NI]
//   | runit[NR] roff[NR] rimp[NR] | code[NC]
// An import names its definition as (unit, ordinal) and also carries the definition's 64-bit fingerprint,
// so the same job can be linked either way. A relocation is a 32-bit field at roff in its unit that
// receives  address(import) - address(field);  units are laid out in order, each aligned to 64 bytes.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static uint64_t rs = 88172645463325252ull;
static inline uint64_t rnd(void){ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void put(const void *p, size_t n) { const char *c = p; while (n) { ssize_t w = write(1, c, n); if (w <= 0) _exit(1); c += w; n -= w; } }
int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "gen")) {
    enum { D = 500, R = 1500 }; uint32_t U = atoi(argv[2]), NE = U * D, NI = U * R;
    uint32_t *usize = malloc(4 * U), *uexp = malloc(4 * U), *exunit = malloc(4 * NE), *exoff = malloc(4 * NE), *efp1 = malloc(4 * NE), *efp2 = malloc(4 * NE);
    uint32_t *imu = malloc(4 * NI), *imo = malloc(4 * NI), *ifp1 = malloc(4 * NI), *ifp2 = malloc(4 * NI); uint64_t NC = 0, NR = 0;
    for (uint32_t u = 0; u < U; u++) { usize[u] = (65536 + rnd() % 131072) & ~3u; uexp[u] = D; NC += usize[u]; }
    for (uint32_t e = 0; e < NE; e++) { uint32_t u = e / D; exunit[e] = u; exoff[e] = (uint32_t)((uint64_t)(e % D) * usize[u] / D); uint64_t f = rnd(); efp1[e] = (uint32_t)f & 0x7fffffff; efp2[e] = f >> 33; }
    for (uint32_t i = 0; i < NI; i++) { uint64_t r = rnd(); uint32_t e = (r % 8 ? r >> 8 : (r >> 8) % 4096 * 37) % NE; imu[i] = e / D; imo[i] = e % D; ifp1[i] = efp1[e]; ifp2[i] = efp2[e]; }
    uint32_t cap = (uint32_t)(NC / 16), *runit = malloc(4ull * cap), *roff = malloc(4ull * cap), *rimp = malloc(4ull * cap);
    for (uint32_t u = 0; u < U; u++) for (uint32_t o = rnd() % 16; o + 4 <= usize[u]; o += 8 + rnd() % 27) { runit[NR] = u; roff[NR] = o; rimp[NR] = u * R + rnd() % R; NR++; }
    uint32_t hdr[5] = {U, NE, NI, (uint32_t)NR, (uint32_t)NC};
    put(hdr, 20); put(usize, 4 * U); put(uexp, 4 * U); put(exunit, 4ull * NE); put(exoff, 4ull * NE); put(efp1, 4ull * NE); put(efp2, 4ull * NE);
    put(imu, 4ull * NI); put(imo, 4ull * NI); put(ifp1, 4ull * NI); put(ifp2, 4ull * NI); put(runit, 4 * NR); put(roff, 4 * NR); put(rimp, 4 * NR);
    uint64_t *buf = malloc(1 << 20); for (uint64_t done = 0; done < NC;) { for (int i = 0; i < 1 << 17; i++) buf[i] = rnd(); uint64_t n = NC - done < 1 << 20 ? NC - done : 1 << 20; put(buf, n); done += n; }
    return 0; }
  // ---- ref: the obvious scalar linker. mmap the job, lay out, resolve imports by (unit, ordinal), copy and patch unit by unit.
  struct stat sb; if (fstat(0, &sb)) return 1;
  const uint32_t *f = mmap(0, sb.st_size, PROT_READ, MAP_PRIVATE, 0, 0); if (f == MAP_FAILED) return 1;
  uint32_t U = f[0], NE = f[1], NI = f[2], NR = f[3];
  const uint32_t *usize = f + 5, *uexp = usize + U, *exunit = uexp + U, *exoff = exunit + NE, *imu = exoff + 3ull * NE, *imo = imu + NI, *runit = imu + 4ull * NI, *roff = runit + NR, *rimp = roff + NR;
  const uint8_t *code = (const uint8_t *)(rimp + NR);
  uint32_t *ubase = malloc(4 * (U + 1)), *inb = malloc(4 * (U + 1)), *exbase = malloc(4 * (U + 1)); ubase[0] = inb[0] = exbase[0] = 0;
  for (uint32_t u = 0; u < U; u++) { ubase[u + 1] = ubase[u] + ((usize[u] + 63) & ~63u); inb[u + 1] = inb[u] + usize[u]; exbase[u + 1] = exbase[u] + uexp[u]; }
  uint32_t *imaddr = malloc(4ull * NI);
  for (uint32_t i = 0; i < NI; i++) { uint32_t e = exbase[imu[i]] + imo[i]; imaddr[i] = ubase[exunit[e]] + exoff[e]; }
  uint8_t *out = calloc(ubase[U] + 64, 1); uint32_t k = 0;
  for (uint32_t u = 0; u < U; u++) { uint8_t *o = out + ubase[u]; memcpy(o, code + inb[u], usize[u]);
    for (; k < NR && runit[k] == u; k++) { uint32_t v = imaddr[rimp[k]] - (ubase[u] + roff[k]); uint8_t *q = o + roff[k];
      q[0] = v; q[1] = v >> 8; q[2] = v >> 16; q[3] = v >> 24; } }      // (four assignments, not memcpy: gcc's fortified memcpy made this a call per relocation; fourth audit)
  put(out, ubase[U]); return 0; }
