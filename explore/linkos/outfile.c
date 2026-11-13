// outfile.c: what does it cost to get 110 MB of linked image into a file? Each variant fills the file with
// 8 threads copying from warm anonymous memory (the stand-in for "copy + relocate"), and is timed from open to
// the last byte written, then to close/munmap. Best of 5.   ./outfile <dir> [MB=110] [threads=8]
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (t.tv_sec + t.tv_nsec * 1e-9) * 1e3; }
static size_t N; static int T; static char *src, *dst; static int fd, mode;
static void *work(void *p) {
  long id = (long)p; size_t per = N / T / 4096 * 4096, o = per * id, n = id == T - 1 ? N - o : per;
  if (mode == 1) { for (size_t k = 0; k < n; k += 1 << 20) { size_t m = n - k < 1 << 20 ? n - k : 1 << 20; if (pwrite(fd, src + o + k, m, o + k) != (ssize_t)m) abort(); } }
  else if (mode == 2) { madvise(dst + o, n, 23); memcpy(dst + o, src + o, n); }
  else memcpy(dst + o, src + o, n);
  return 0;
}
static void fill(void) { pthread_t th[64]; for (long i = 0; i < T; i++) pthread_create(&th[i], 0, work, (void *)i); for (int i = 0; i < T; i++) pthread_join(th[i], 0); }
int main(int argc, char **argv) {
  const char *dir = argv[1]; N = (size_t)(argc > 2 ? atoi(argv[2]) : 110) << 20; T = argc > 3 ? atoi(argv[3]) : 8;
  src = mmap(0, N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); memset(src, 3, N);
  char path[4096]; snprintf(path, sizeof path, "%s/outfile.tmp", dir);
  const char *names[] = {"new file, mmap shared", "new file, pwrite 1MB chunks", "new file, mmap + MADV_POPULATE_WRITE per thread",
                         "new file, fallocate + mmap", "existing file (cached), mmap, no truncate", "existing file (cached), pwrite, no truncate",
                         "existing file, mmap + MAP_POPULATE", "anon memory (no file at all)", "anon + populate_write per thread",
                         "unlink old + new file, mmap shared"};
  unsigned vmask = argc > 4 ? strtoul(argv[4], 0, 0) : ~0u;   // which variants (bit v)
  for (int v = 0; v < 10; v++) {
    if (!(vmask >> v & 1)) continue;
    double best = 1e300, bestc = 1e300, bestu = 1e300;
    for (int r = 0; r < 5; r++) {
      int existing = v == 4 || v == 5 || v == 6;
      if (!existing && v != 9) unlink(path);
      if (v == 9) { /* the old output exists, as in a rebuild */ }
      double t0 = now();
      if (v == 9) unlink(path);
      double tu = now() - t0;
      if (v == 7 || v == 8) { dst = mmap(0, N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); mode = v == 8 ? 2 : 0; fill(); double t1 = now(); munmap(dst, N); double t2 = now();
        if (t1 - t0 < best) best = t1 - t0; if (t2 - t1 < bestc) bestc = t2 - t1; continue; }
      fd = open(path, O_RDWR | O_CREAT | (existing ? 0 : O_TRUNC), 0755);
      if (!existing && ftruncate(fd, N)) abort();
      if (v == 3 && fallocate(fd, 0, 0, N)) perror("fallocate");
      mode = v == 1 || v == 5 ? 1 : v == 2 ? 2 : 0;
      if (mode != 1) dst = mmap(0, N, PROT_READ | PROT_WRITE, MAP_SHARED | (v == 6 ? MAP_POPULATE : 0), fd, 0);
      fill();
      double t1 = now();
      if (mode != 1) munmap(dst, N);
      close(fd);
      double t2 = now();
      if (t1 - t0 < best) best = t1 - t0; if (t2 - t1 < bestc) bestc = t2 - t1; if (tu < bestu) bestu = tu;
    }
    printf("%-52s fill %6.1f ms   close/unmap %5.1f ms%s", names[v], best, bestc, v == 9 ? "" : "\n");
    if (v == 9) printf("   (unlink of the old one: %.1f ms of the fill)\n", bestu);
  }
  unlink(path);
  return 0;
}
