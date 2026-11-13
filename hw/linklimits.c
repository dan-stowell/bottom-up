// linklimits.c: the machine limits the link floors in FLOOR.md divide by. Prints "key value" lines.
//   gcc -O2 -march=native -pthread -o build/linklimits hw/linklimits.c && build/linklimits <scratch dir on the disk>
// Every figure is the best of several runs (the host is noisy: the best run is the least disturbed one).
//   copy_<level>_<n>t      bytes/s copied by n threads, each inside its own block that fits the level
//   read_dram_<n>t / write_dram_<n>t   bytes/s of a read-only stream (summed) and of a non-temporal fill
//   instr_<n>t             instructions/s of a dependency-free scalar loop (the dispatch limit)
//   rand_<size>_<n>t       independent random 8-byte loads/s from a table of that size (memory-level parallelism)
//   chase_<size>           dependent loads/s (one pointer chain: the latency of a miss)
//   anon_touch_<n>t        pages/s of first-touch on fresh anonymous memory (4 KiB pages)
//   anon_populate_<n>t     the same through madvise(MADV_POPULATE_WRITE)
//   huge_touch_1t          bytes/s of first touch on hugetlb 2 MiB pages (if any are reserved)
//   filemap_write_<fs>_<n>t  pages/s of first-touch through a MAP_SHARED mapping of a new file
//   pwrite_<fs>_<n>t       bytes/s of pwrite() of warm memory into a new file
//   filemap_read_<n>t      pages/s reading a page-cached file through a private mapping (fault-around on)
//   filemap_readpop_<n>t   the same with MAP_POPULATE
//   munmap_file / munmap_anon   pages/s of tearing the mapping down
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

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

typedef struct { int id, n; void (*f)(int id, void *arg); void *arg; pthread_barrier_t *go; double t0, t1; } job;
static void *tramp(void *p) {
  job *j = p;
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(j->id % 8, &s); pthread_setaffinity_np(pthread_self(), sizeof s, &s);
  pthread_barrier_wait(j->go);
  j->t0 = now(); j->f(j->id, j->arg); j->t1 = now();
  return 0;
}
// run f on n threads, started together; returns the wall time from the first start to the last finish
static double par(int n, void (*f)(int, void *), void *arg) {
  pthread_t th[64]; job js[64]; pthread_barrier_t go; pthread_barrier_init(&go, 0, n);
  for (int i = 0; i < n; i++) { js[i] = (job){i, n, f, arg, &go, 0, 0}; pthread_create(&th[i], 0, tramp, &js[i]); }
  double t0 = 1e300, t1 = 0;
  for (int i = 0; i < n; i++) { pthread_join(th[i], 0); if (js[i].t0 < t0) t0 = js[i].t0; if (js[i].t1 > t1) t1 = js[i].t1; }
  return t1 - t0;
}
static int threads[] = {1, 2, 4, 8};

// ---- copy
#include <immintrin.h>
static struct { size_t block; size_t total; char *mem; } cp;
__m256i rd_sink256[64];   // (not static: the sums must be stored)
static void copy_work(int id, void *arg) {
  char *a = cp.mem + (size_t)id * 2 * cp.block, *b = a + cp.block;
  for (size_t done = 0; done < cp.total; done += cp.block) { memcpy(b, a, cp.block); __asm__ volatile("" : : "r"(b) : "memory"); }
}
static void copy_level(const char *name, size_t block, size_t total) {
  cp.block = block; cp.total = total; cp.mem = mmap(0, 16 * block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memset(cp.mem, 1, 16 * block);
  for (int k = 0; k < 4; k++) {
    int n = threads[k]; double best = 1e300;
    for (int r = 0; r < 7; r++) { double t = par(n, copy_work, 0); if (t < best) best = t; }
    printf("copy_%s_%dt %.0f\n", name, n, (double)total * n / best);
  }
  munmap(cp.mem, 16 * block);
}

// ---- one-directional streams over DRAM-sized blocks: read (sum) and write (non-temporal fill)
#include <immintrin.h>
static void read_work(int id, void *arg) {
  const char *a = cp.mem + (size_t)id * 2 * cp.block; __m256i s = _mm256_setzero_si256();
  for (size_t done = 0; done < cp.total; done += cp.block)
    for (size_t o = 0; o < cp.block; o += 64) { s = _mm256_add_epi64(s, _mm256_load_si256((const __m256i *)(a + o))); s = _mm256_add_epi64(s, _mm256_load_si256((const __m256i *)(a + o + 32))); }
  rd_sink256[id] = s; __asm__ volatile("" : : "x"(s) : "memory");
}
static void write_work(int id, void *arg) {
  char *a = cp.mem + (size_t)id * 2 * cp.block; __m256i v = _mm256_set1_epi8(5);
  for (size_t done = 0; done < cp.total; done += cp.block) {
    for (size_t o = 0; o < cp.block; o += 64) { _mm256_stream_si256((__m256i *)(a + o), v); _mm256_stream_si256((__m256i *)(a + o + 32), v); }
    _mm_sfence();
  }
}
static void stream_dram(void) {
  cp.block = 256 << 20; cp.total = 2ull << 30; cp.mem = mmap(0, 16 * cp.block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memset(cp.mem, 1, 16 * cp.block);
  for (int k = 0; k < 4; k++) {
    int n = threads[k]; double br = 1e300, bw = 1e300;
    for (int r = 0; r < 7; r++) { double t = par(n, read_work, 0); if (t < br) br = t; t = par(n, write_work, 0); if (t < bw) bw = t; }
    printf("read_dram_%dt %.0f\nwrite_dram_%dt %.0f\n", n, (double)cp.total * n / br, n, (double)cp.total * n / bw);
  }
  munmap(cp.mem, 16 * cp.block);
}

// ---- instructions
static uint64_t instr_iters = 400000000ull;
static void instr_work(int id, void *arg) {
  uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, n = instr_iters;
  // 6 instructions per iteration, five independent one-cycle chains and the fused loop branch
  __asm__ volatile(
    "1: add $1, %0\n add $1, %1\n add $1, %2\n add $1, %3\n add $1, %4\n dec %5\n jnz 1b\n"
    : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(n));
}
static void instr_work2(int id, void *arg) {
  uint64_t r[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, n = instr_iters / 2;
  // 12 instructions per iteration: eleven independent adds (some to the same register twice) and the branch
  __asm__ volatile(
    "1: add $1, %%r8\n add $1, %%r9\n add $1, %%r10\n add $1, %%r11\n add $1, %%rax\n add $1, %%rcx\n"
    " add $1, %%rdx\n add $1, %%rsi\n add $1, %%rdi\n add $1, %%r12\n dec %0\n jnz 1b\n"
    : "+r"(n) : : "r8", "r9", "r10", "r11", "rax", "rcx", "rdx", "rsi", "rdi", "r12");
  (void)r;
}

// ---- random loads
static struct { uint64_t *tab; size_t mask; size_t n; uint64_t sink[64]; } rd;
static void rand_work(int id, void *arg) {
  uint64_t s = 0, base = (uint64_t)id * 0x632BE59BD9B4E019ull;
  for (size_t i = 0; i < rd.n; i++) {
    uint64_t x = (base + i) * 0x9E3779B97F4A7C15ull;   // a counter through a multiplicative hash: no index depends on another
    s += rd.tab[(x >> 20) & rd.mask];
  }
  rd.sink[id] = s;
}
static void chase_work(int id, void *arg) {
  uint64_t p = id;
  for (size_t i = 0; i < rd.n; i++) p = rd.tab[p & rd.mask];
  rd.sink[id] = p;
}

// ---- pages
#define PG 4096
static struct { char *mem; size_t bytes; int fd; int mode; } pg;
static void touch_work(int id, void *arg) {
  job *unused = 0; (void)unused;
  int n = *(int *)arg; size_t per = pg.bytes / n / PG * PG; char *a = pg.mem + per * id;
  if (pg.mode == 1) { madvise(a, per, 23 /* MADV_POPULATE_WRITE */); return; }
  if (pg.mode == 2) { uint64_t s = 0; for (size_t o = 0; o < per; o += PG) s += a[o]; rd.sink[id] = s; return; }
  if (pg.mode == 3) { static __thread char *buf; if (!buf) { buf = malloc(1 << 20); memset(buf, 7, 1 << 20); }
    for (size_t o = 0; o < per; o += 1 << 20) if (pwrite(pg.fd, buf, 1 << 20, per * id + o) < 0) abort(); return; }
  for (size_t o = 0; o < per; o += PG) a[o] = 1;
}

// ---- opening inputs: open + fstat + mmap + close of page-cached files (argv[2]: a list of paths)
#include <sys/stat.h>
static struct { char **path; int n; int next; } op;
static void open_work(int id, void *arg) {
  for (;;) {
    int i = __atomic_fetch_add(&op.next, 1, __ATOMIC_RELAXED);
    if (i >= op.n) return;
    int fd = open(op.path[i], O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) continue;
    void *m = mmap(0, st.st_size ? st.st_size : 1, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    rd.sink[id] += *(volatile char *)m;      // the first page: its header
  }
}
static void open_rate(const char *list) {
  FILE *f = fopen(list, "r"); if (!f) return;
  char line[8192]; int cap = 1 << 16; op.path = malloc(cap * sizeof(char *)); op.n = 0;
  while (fgets(line, sizeof line, f) && op.n < cap) { line[strcspn(line, "\n")] = 0; op.path[op.n++] = strdup(line); }
  fclose(f);
  for (int k = 0; k < 4; k++) {
    int n = threads[k]; double best = 1e300;
    for (int r = 0; r < 5; r++) { op.next = 0; double d = par(n, open_work, 0); if (d < best) best = d; }
    printf("openmap_%dt %.0f\n", n, op.n / best);
  }
}

int main(int argc, char **argv) {
  const char *disk = argc > 1 ? argv[1] : ".";
  if (argc > 2) open_rate(argv[2]);
  if (argc > 3) return 0;      // (any third argument: only the open rate)
  copy_level("l1", 12 << 10, 4ull << 30);
  copy_level("l2", 256 << 10, 4ull << 30);
  copy_level("l3", 1 << 20, 4ull << 30);    // 8 threads x 2 MiB = half of the 32 MiB L3
  copy_level("dram", 256 << 20, 2ull << 30);
  stream_dram();

  for (int k = 0; k < 4; k++) {
    int n = threads[k]; double best = 1e300, best2 = 1e300;
    for (int r = 0; r < 5; r++) { double t = par(n, instr_work, 0); if (t < best) best = t; t = par(n, instr_work2, 0); if (t < best2) best2 = t; }
    double a = 7.0 * instr_iters * n / best, b = 12.0 * (instr_iters / 2) * n / best2;
    printf("instr_%dt %.0f\n", n, a > b ? a : b);
  }

  size_t sizes[] = {16 << 20, 64 << 20, 512 << 20};
  const char *names[] = {"16m", "64m", "512m"};
  for (int s = 0; s < 3; s++) {
    size_t n = sizes[s] / 8;
    rd.tab = mmap(0, sizes[s], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t x = 1;
    for (size_t i = 0; i < n; i++) rd.tab[i] = i;
    for (size_t i = n - 1; i > 0; i--) {              // Sattolo: a single cycle through every entry
      x ^= x << 13; x ^= x >> 7; x ^= x << 17;
      size_t k = x % i; uint64_t t = rd.tab[i]; rd.tab[i] = rd.tab[k]; rd.tab[k] = t;
    }
    rd.mask = n - 1; rd.n = 20000000;
    for (int k = 0; k < 4; k++) {
      int t = threads[k]; double best = 1e300;
      for (int r = 0; r < 5; r++) { double d = par(t, rand_work, 0); if (d < best) best = d; }
      printf("rand_%s_%dt %.0f\n", names[s], t, (double)rd.n * t / best);
    }
    rd.n = 3000000;
    double best = 1e300;
    for (int r = 0; r < 3; r++) { double d = par(1, chase_work, 0); if (d < best) best = d; }
    printf("chase_%s %.0f\n", names[s], (double)rd.n / best);
    munmap(rd.tab, sizes[s]);
  }

  // anonymous first touch, and its teardown
  pg.bytes = 1ull << 30;
  for (int mode = 0; mode < 2; mode++)
    for (int k = 0; k < 4; k++) {
      int n = threads[k]; double best = 1e300, bestu = 1e300;
      for (int r = 0; r < 3; r++) {
        pg.mem = mmap(0, pg.bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); pg.mode = mode;
        double d = par(n, touch_work, &n); if (d < best) best = d;
        double t0 = now(); munmap(pg.mem, pg.bytes); d = now() - t0; if (d < bestu) bestu = d;
      }
      printf("%s_%dt %.0f\n", mode ? "anon_populate" : "anon_touch", n, pg.bytes / PG / best);
      if (mode == 0 && n == 1) printf("munmap_anon %.0f\n", pg.bytes / PG / bestu);
    }
  // hugetlb
  { size_t hb = 256 << 20; char *h = mmap(0, hb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (h != MAP_FAILED) { double t0 = now(); for (size_t o = 0; o < hb; o += 2 << 20) h[o] = 1; printf("huge_touch_1t %.0f\n", hb / (now() - t0)); munmap(h, hb); }
    else printf("huge_touch_1t 0\n"); }

  // a new file: written through a shared mapping, and with pwrite; on the disk and on tmpfs
  const char *fss[] = {disk, "/dev/shm"}; const char *fsn[] = {"disk", "shm"};
  for (int f = 0; f < 2; f++) {
    char path[4096]; snprintf(path, sizeof path, "%s/linklimits.tmp", fss[f]);
    pg.bytes = 512ull << 20;
    for (int mode = 0; mode <= 3; mode += 3)
      for (int k = 0; k < 4; k++) {
        int n = threads[k]; double best = 1e300;
        for (int r = 0; r < 3; r++) {
          unlink(path); pg.fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
          if (pg.fd < 0 || ftruncate(pg.fd, pg.bytes)) { perror(path); return 1; }
          pg.mode = mode;
          if (mode == 0) pg.mem = mmap(0, pg.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pg.fd, 0);
          double d = par(n, touch_work, &n); if (d < best) best = d;
          if (mode == 0) munmap(pg.mem, pg.bytes);
          close(pg.fd);
        }
        if (mode == 0) printf("filemap_write_%s_%dt %.0f\n", fsn[f], n, pg.bytes / PG / best);
        else printf("pwrite_%s_%dt %.0f\n", fsn[f], n, pg.bytes / best);
      }
    if (f == 0) {   // the file is now in the page cache: read it through a private mapping
      for (int pop = 0; pop < 2; pop++)
        for (int k = 0; k < 4; k++) {
          int n = threads[k]; double best = 1e300, bestu = 1e300;
          for (int r = 0; r < 3; r++) {
            int fd = open(path, O_RDONLY); pg.mode = 2;
            double t0 = now();
            pg.mem = mmap(0, pg.bytes, PROT_READ, MAP_PRIVATE | (pop && n == 1 ? MAP_POPULATE : 0), fd, 0);
            double d;
            if (pop && n > 1) { pg.mode = 4; }
            if (pop && n == 1) d = now() - t0;
            else if (pop) { pg.mode = 5; d = 0; }
            else d = par(n, touch_work, &n);
            if (pop && n > 1) continue;
            if (d < best) best = d;
            t0 = now(); munmap(pg.mem, pg.bytes); d = now() - t0; if (d < bestu) bestu = d;
            close(fd);
          }
          if (pop && n > 1) continue;
          printf("%s_%dt %.0f\n", pop ? "filemap_readpop" : "filemap_read", n, pg.bytes / PG / best);
          if (!pop && n == 1) printf("munmap_file %.0f\n", pg.bytes / PG / bestu);
        }
    }
    unlink(path);
  }
  return 0;
}
