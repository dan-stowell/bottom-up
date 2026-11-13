// lazy.c: a spike of the front half of a Go link (load, resolve, reachability) in the shape IDEAS.md #5
// proposes, on real objects, to see how close it gets to the floor before it goes into hlink.
//
//   gcc -O2 -g -march=native -pthread -o build/lazy explore/linkos/lazy.c
//   build/lazy -importcfg importcfg.link [-j N] [-dumplive file]
//
// What is different from fclink's eager design:
//   * Only symbols that are defined by name (the NonPkgDef block: 0.55 M of MinIO's 5.2 M slots) are joined
//     up front, into one table partitioned by hash so that it is built without locks.
//   * References by name (0.85 M) probe that table; the few that miss name package-indexed definitions (or
//     nothing): those names form a small set that one pass over the package-indexed definitions resolves.
//   * Content-addressed symbols (2.7 M slots) are not resolved at all until the flood reaches one. Then the
//     slot is interned by its content hash in a lock-free table, and the class's representative is the
//     lowest reached slot. Every reached member is flooded (not only the first one seen), so the set of
//     reached slots, and with it every representative, does not depend on thread timing.
//   * References by package index need no resolution.
// Names are compared by a 128-bit hash and never by their bytes.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <immintrin.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32; typedef int64_t i64;
#define die(...) do { fprintf(stderr, "lazy: " __VA_ARGS__); fputc('\n', stderr); exit(2); } while (0)
static inline u32 rd32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 rd64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u16 rd16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static double cpu_ms(int who) { struct rusage r; getrusage(RUSAGE_SELF, &r); struct timeval v = who ? r.ru_stime : r.ru_utime; return v.tv_sec * 1e3 + v.tv_usec * 1e-3; }

// ---- constants of the object format (cmd/internal/goobj, go1.27) and of objabi
enum { BlkAutolib, BlkPkgIdx, BlkFile, BlkSymdef, BlkHashed64def, BlkHasheddef, BlkNonpkgdef, BlkNonpkgref, BlkRefFlags, BlkHash64, BlkHash,
       BlkRelocIdx, BlkAuxIdx, BlkDataIdx, BlkReloc, BlkAux, BlkData, BlkRefName, BlkEnd, NBlk };
#define SymSize 21
#define RelocSize 23
#define AuxSize 9
#define PkgIdxNone 0x7fffffffu
#define PkgIdxHashed64 0x7ffffffeu
#define PkgIdxHashed 0x7ffffffdu
#define PkgIdxBuiltin 0x7ffffffcu
#define PkgIdxSelf 0x7ffffffbu
#define SymABIstatic 0xffff
enum { SymFlagDupok = 1, SymFlagLocal = 2, SymFlagTypelink = 4, SymFlagLeaf = 8, SymFlagNoSplit = 16, SymFlagReflectMethod = 32, SymFlagGoType = 64 };
enum { SymFlagUsedInIface = 1, SymFlagItab = 2 };
enum { AuxGotype, AuxFuncInfo, AuxFuncdata, AuxDwarfInfo, AuxDwarfLoc, AuxDwarfRanges, AuxDwarfLines, AuxPcsp, AuxPcfile, AuxPcline, AuxPcinline, AuxPcdata };
enum { R_USETYPE = 22, R_USEIFACE = 23, R_USEIFACEMETHOD = 24, R_USENAMEDMETHOD = 25, R_METHODOFF = 26, R_INITORDER = 102, R_WEAK = 0x8000 };
enum { K_STEXT = 1, K_STEXTFIPS = 2 };
#define TFlagOff 20
#define TFlagUncommon 1
static const u8 rtypeSize[32] = {48,48,48,48,48,48,48,48,48,48,48,48,48,48,48,48,48, 72,64,56,80,136,56,56,48,80,48};
#include "builtins.h"

// ---- threads: a pool that spins between jobs (a link lasts milliseconds)
static int nthreads = 8;
typedef void (*parfn)(int w, int i);
static struct { parfn f; int n; _Alignas(64) volatile int next; _Alignas(64) volatile u64 gen; volatile int done; volatile int quit; } pool;
static void pool_run(int w) {
  for (;;) { int i = __atomic_fetch_add(&pool.next, 1, __ATOMIC_RELAXED); if (i >= pool.n) break; pool.f(w, i); }
}
static void *pool_thread(void *arg) {
  int w = (int)(long)arg; u64 seen = 0;
  for (;;) {
    u64 g;
    while ((g = __atomic_load_n(&pool.gen, __ATOMIC_ACQUIRE)) == seen) { if (pool.quit) return 0; _mm_pause(); }
    seen = g; pool_run(w);
    __atomic_fetch_add(&pool.done, 1, __ATOMIC_RELEASE);
  }
}
static void pool_start(void) { for (long w = 1; w < nthreads; w++) { pthread_t t; pthread_create(&t, 0, pool_thread, (void *)w); } }
static void par(int n, parfn f) {
  if (n <= 0) return;
  if (n == 1 || nthreads == 1) { for (int i = 0; i < n; i++) f(0, i); return; }
  pool.f = f; pool.n = n; pool.next = 0; pool.done = 0;
  __atomic_fetch_add(&pool.gen, 1, __ATOMIC_RELEASE);
  pool_run(0);
  while (__atomic_load_n(&pool.done, __ATOMIC_ACQUIRE) != nthreads - 1) _mm_pause();
}
static void *big(size_t n) {   // zeroed memory for a big array: huge pages if the machine has some reserved
  n = (n + (2 << 20) - 1) & ~(size_t)((2 << 20) - 1);
  void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (p == MAP_FAILED) p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("out of memory");
  return p;
}
typedef struct { u32 *v; u32 n, cap; } vec;
static inline void push(vec *v, u32 x) { if (v->n == v->cap) { v->cap = v->cap ? 2 * v->cap : 4096; v->v = realloc(v->v, 4 * (size_t)v->cap); } v->v[v->n++] = x; }

// ---- objects
typedef struct object {
  const u8 *b;             // the object (after the "go object" text header)
  const u8 *sym, *relidx, *auxidx, *dataidx, *rel, *aux, *data, *hash64, *hash;
  u32 base;                // global id of slot 0
  u32 ndef, nh64, nh, nnonpkg, nref, nalldef, total;
  u32 h64lo, hlo, nplo, reflo;   // first slot index of each block
  u32 npkg; u32 *pkgbase;
  const char *pkg, *file; u32 flags;
} object;
typedef struct { const char *pkg, *file; const u8 *map; size_t size; object *objs; int nobj; } lib;
static lib *libs; static int nlibs;
static object **objs; static int nobjs;
static u32 nslots; static u32 *objpage;
#define PAGESHIFT 9
typedef struct { object *o; u32 lo, hi; } unit;
static unit *units; static int nunits;
#define UNITSLOTS 8192

static inline object *obj_of(u32 gid, u32 *j) {
  u32 k = objpage[gid >> PAGESHIFT];
  for (;; k++) { object *o = objs[k]; if (gid - o->base < o->total) { *j = gid - o->base; return o; } }
}
static inline const u8 *symrec(const object *o, u32 j) { return o->sym + (size_t)j * SymSize; }
static inline u32 s_namelen(const u8 *s) { return rd32(s); }
static inline const u8 *s_name(const object *o, const u8 *s) { return o->b + rd32(s + 4); }
static inline u16 s_abi(const u8 *s) { return rd16(s + 8); }
static inline u8 s_type(const u8 *s) { return s[10]; }
static inline u8 s_flag(const u8 *s) { return s[11]; }
static inline u8 s_flag2(const u8 *s) { return s[12]; }
static inline u32 s_siz(const u8 *s) { return rd32(s + 13); }
static inline const u8 *relocs(const object *o, u32 j, u32 *n) { u32 a = rd32(o->relidx + 4 * (size_t)j), b = rd32(o->relidx + 4 * (size_t)j + 4); *n = b - a; return o->rel + (size_t)a * RelocSize; }
static inline const u8 *auxs(const object *o, u32 j, u32 *n) { u32 a = rd32(o->auxidx + 4 * (size_t)j), b = rd32(o->auxidx + 4 * (size_t)j + 4); *n = b - a; return o->aux + (size_t)a * AuxSize; }
static inline const u8 *data(const object *o, u32 j, u32 *n) { u32 a = rd32(o->dataidx + 4 * (size_t)j), b = rd32(o->dataidx + 4 * (size_t)j + 4); *n = b - a; return o->data + a; }
static inline i32 r_off(const u8 *r) { return (i32)rd32(r); }
static inline u16 r_type(const u8 *r) { return rd16(r + 5); }
static inline i64 r_add(const u8 *r) { return (i64)rd64(r + 7); }

static u32 *canon;        // per slot defined or referenced by name: its canonical definition; content-addressed slots: set after the flood
static u8 *state;         // per slot
enum { stLive = 1, stUsedInIface = 2, stIfaceDone = 4, stPC = 8, stFuncdata = 16, stNameNeeded = 32, stUndef = 64, stDup = 128 };
static u32 builtins[NBUILTIN];

// ref: the slot a symbol reference names (not yet its canonical definition); 0 for the nil reference
static inline u32 ref(const object *o, u32 pkg, u32 idx) {
  if (pkg == PkgIdxSelf) return o->base + idx;
  if (pkg == PkgIdxNone) return o->base + o->nplo + idx;
  if (pkg == PkgIdxHashed) return o->base + o->hlo + idx;
  if (pkg == PkgIdxHashed64) return o->base + o->h64lo + idx;
  if (pkg == PkgIdxBuiltin) return builtins[idx];
  if (pkg == 0) return 0;
  u32 b = o->pkgbase[pkg];
  if (b == ~0u) die("%s: reference to package index %u, which is not in the importcfg", o->file, pkg);
  return b + idx;
}
static inline u32 refat(const object *o, const u8 *p) { return ref(o, rd32(p), rd32(p + 4)); }
// can: the canonical symbol of a slot. Content-addressed slots stand for themselves until the flood is over.
static inline u32 can(const object *o, u32 pkg, u32 idx) {
  u32 g = ref(o, pkg, idx);
  if (pkg == PkgIdxNone) return canon[g];
  return g;
}
static inline u32 canat(const object *o, const u8 *p) { return can(o, rd32(p), rd32(p + 4)); }
static inline int is_def(u32 c) { return c != 0 && !(state[c] & stUndef); }

// ---- name hashing: 128 bits, 16 bytes a step
static inline u64 mum(u64 a, u64 b) { __uint128_t r = (__uint128_t)a * b; return (u64)r ^ (u64)(r >> 64); }
typedef struct { u64 k1, k2; } key;
static inline key namekey(const u8 *p, size_t n, u16 abi) {
  u64 a = 0x9E3779B97F4A7C15ull ^ n, b = 0xD6E8FEB86659FD93ull ^ ((u64)abi * 0xA24BAED4963EE407ull);
  while (n > 16) { u64 x = rd64(p), y = rd64(p + 8); a = mum(x ^ a, y ^ 0xE7037ED1A0B428DBull); b = mum(x ^ 0x8EBC6AF09C88C6E3ull, y ^ b); p += 16; n -= 16; }
  // the last 1..16 bytes (reads up to 15 bytes past the name: always inside the mapped object)
  u64 x = rd64(p), y = rd64(p + 8);
  if (n <= 8) { x = n == 8 ? x : x & ((1ull << (8 * n)) - 1); y = 0; } else if (n < 16) y &= (1ull << (8 * (n - 8))) - 1;
  a = mum(x ^ a, y ^ 0x589965CC75374CC3ull); b = mum(x ^ 0x1D8E4E27C47D124Full, y ^ b);
  key k = {mum(a, b ^ 0xEB44ACCAB455D165ull), mum(a ^ 0x4F1BBCDCBFA53E0Aull, b)};
  if (k.k1 == 0) k.k1 = 1;
  return k;
}

// ---- load
static const u8 *go_object(const u8 *b, size_t n) {
  if (n < 16 || memcmp(b, "go object ", 10)) return 0;
  const u8 *e = memmem(b, n < 4096 ? n : 4096, "\n!\n", 3);
  if (!e) e = memmem(b, n, "\n!\n", 3);
  return e ? e + 3 : 0;
}
static int new_object(lib *l, object *o, const u8 *body, size_t n) {
  const u8 *b = go_object(body, n);
  if (!b) return 0;
  if (memcmp(b, "\0go120ld", 8)) die("%s: not a Go object of this toolchain", l->file);
  memset(o, 0, sizeof *o);
  u32 off[NBlk]; for (int i = 0; i < NBlk; i++) off[i] = rd32(b + 20 + 4 * i);
  o->b = b; o->pkg = l->pkg; o->file = l->file; o->flags = rd32(b + 16);
  o->ndef = (off[BlkHashed64def] - off[BlkSymdef]) / SymSize; o->nh64 = (off[BlkHasheddef] - off[BlkHashed64def]) / SymSize;
  o->nh = (off[BlkNonpkgdef] - off[BlkHasheddef]) / SymSize; o->nnonpkg = (off[BlkNonpkgref] - off[BlkNonpkgdef]) / SymSize;
  o->nref = (off[BlkRefFlags] - off[BlkNonpkgref]) / SymSize;
  o->h64lo = o->ndef; o->hlo = o->h64lo + o->nh64; o->nplo = o->hlo + o->nh; o->reflo = o->nplo + o->nnonpkg;
  o->nalldef = o->reflo; o->total = o->nalldef + o->nref;
  o->sym = b + off[BlkSymdef]; o->relidx = b + off[BlkRelocIdx]; o->auxidx = b + off[BlkAuxIdx]; o->dataidx = b + off[BlkDataIdx];
  o->rel = b + off[BlkReloc]; o->aux = b + off[BlkAux]; o->data = b + off[BlkData]; o->hash64 = b + off[BlkHash64]; o->hash = b + off[BlkHash];
  o->npkg = (off[BlkFile] - off[BlkPkgIdx]) / 8;
  return 1;
}
static void load_lib(int w, int i) {
  lib *l = &libs[i];
  int fd = open(l->file, O_RDONLY); struct stat st;
  if (fd < 0 || fstat(fd, &st)) die("%s: cannot open", l->file);
  l->size = st.st_size; l->map = mmap(0, l->size ? l->size : 1, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
  if (l->map == MAP_FAILED) die("%s: mmap failed", l->file);
  const u8 *b = l->map; int cap = 2; l->objs = malloc(cap * sizeof(object)); l->nobj = 0;
  if (l->size < 8 || memcmp(b, "!<arch>\n", 8)) { if (!new_object(l, &l->objs[0], b, l->size)) die("%s: not a Go object or archive", l->file); l->nobj = 1; return; }
  for (size_t off = 8; off + 60 <= l->size;) {
    const u8 *h = b + off; size_t size = strtoul((const char *)h + 48, 0, 10);
    const u8 *body = h + 60; off += 60 + size + (size & 1);
    if (!memcmp(h, "__.PKGDEF", 9)) continue;
    if (l->nobj == cap) { cap *= 2; l->objs = realloc(l->objs, cap * sizeof(object)); }
    if (new_object(l, &l->objs[l->nobj], body, size)) l->nobj++;
  }
}
// PathToPrefix: how objects spell a package path in symbol names and in their package lists
static char *path_to_prefix(const char *s) {
  size_t n = strlen(s); const char *slash = strrchr(s, '/'); char *out = malloc(3 * n + 1), *q = out;
  for (const char *p = s; *p; p++) {
    u8 c = *p;
    if (c <= ' ' || (c == '.' && (!slash || p > slash)) || c == '%' || c == '"' || c >= 0x7f) { static const char hex[] = "0123456789abcdef"; *q++ = '%'; *q++ = hex[c >> 4]; *q++ = hex[c & 15]; }
    else *q++ = c;
  }
  *q = 0; return out;
}
static struct { key *k; object **o; u32 mask; } primary;
static void primary_put(const char *name, size_t n, object *o) {
  key k = namekey((const u8 *)name, n, 0);
  for (u32 h = k.k1 & primary.mask;; h = (h + 1) & primary.mask) {
    if (!primary.o[h]) { primary.k[h] = k; primary.o[h] = o; return; }
    if (primary.k[h].k1 == k.k1 && primary.k[h].k2 == k.k2) return;
  }
}
static object *primary_get(const u8 *name, size_t n) {
  key k = namekey(name, n, 0);
  for (u32 h = k.k1 & primary.mask;; h = (h + 1) & primary.mask) {
    if (!primary.o[h]) return 0;
    if (primary.k[h].k1 == k.k1 && primary.k[h].k2 == k.k2) return primary.o[h];
  }
}
static void pkg_bases(int w, int i) {
  object *o = objs[i]; const u8 *pk = o->b + rd32(o->b + 20 + 4 * BlkPkgIdx);
  o->pkgbase = malloc(4 * (o->npkg + 1)); o->pkgbase[0] = ~0u;
  for (u32 p = 1; p < o->npkg; p++) {     // (entry 0 is the invalid index)
    u32 len = rd32(pk + 8 * p), off = rd32(pk + 8 * p + 4);
    object *q = primary_get(o->b + off, len);
    o->pkgbase[p] = q ? q->base : ~0u;
  }
}
static void load(const char *cfgfile, const char *mainfile) {
  FILE *f = fopen(cfgfile, "r"); if (!f) die("%s: cannot open", cfgfile);
  static char line[1 << 20]; int cap = 4096; libs = malloc(cap * sizeof(lib)); int havemain = 0;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "packagefile ", 12)) continue;
    char *p = line + 12, *eq = strchr(p, '='); if (!eq) die("bad importcfg line");
    *eq = 0; char *file = eq + 1; file[strcspn(file, "\n")] = 0;
    if (nlibs == cap) { cap *= 2; libs = realloc(libs, cap * sizeof(lib)); }
    int ismain = mainfile && !strcmp(file, mainfile); havemain |= ismain;
    libs[nlibs++] = (lib){ismain ? "main" : strdup(p), strdup(file)};
  }
  fclose(f);
  if (mainfile && !havemain) { libs[nlibs++] = (lib){"main", mainfile}; }
  par(nlibs, load_lib);
  for (int i = 0; i < nlibs; i++) nobjs += libs[i].nobj;
  objs = malloc(nobjs * sizeof(object *)); primary.mask = 1; while (primary.mask < 4u * nlibs) primary.mask <<= 1;
  primary.k = calloc(primary.mask, sizeof(key)); primary.o = calloc(primary.mask, sizeof(object *)); primary.mask--;
  u32 next = 1; int k = 0;
  for (int i = 0; i < nlibs; i++) {
    char *pp = path_to_prefix(libs[i].pkg);
    for (int j = 0; j < libs[i].nobj; j++) {
      object *o = &libs[i].objs[j]; objs[k++] = o; o->base = next; next += o->total;
      primary_put(pp, strlen(pp), o);   // the first object of a package holds its package-indexed symbols
      nunits += o->total / UNITSLOTS + 1;
    }
  }
  nslots = next - 1;
  units = malloc(nunits * sizeof(unit)); nunits = 0;
  for (int i = 0; i < nobjs; i++) for (u32 lo = 0; lo < objs[i]->total || lo == 0; lo += UNITSLOTS) units[nunits++] = (unit){objs[i], lo, lo + UNITSLOTS < objs[i]->total ? lo + UNITSLOTS : objs[i]->total};
  objpage = malloc(4 * ((nslots >> PAGESHIFT) + 2)); k = 0;
  for (u32 p = 0; p < (nslots >> PAGESHIFT) + 2; p++) { u32 g = p << PAGESHIFT; while (k + 1 < nobjs && objs[k]->base + objs[k]->total <= g) k++; objpage[p] = k; }
  par(nobjs, pkg_bases);
}

// ---- resolve: the join by name
// One record per definition by name and per reference by name is scattered by the top bits of its key into
// NPART partitions per worker. Each partition is then grouped by one thread into its own small table (so the
// build takes no lock), package-indexed definitions whose name is in some table join in (a Bloom filter of the
// tables' keys keeps most of them out), and a last pass per partition tells every record's slot its winner.
#define NPART 256
typedef struct { key k; u64 val; } nrec;           // val: rank << 32 | ~gid (the winner is the maximum)
#define CHUNK 340
typedef struct chunk { struct chunk *next; u64 n; nrec r[CHUNK]; } chunk;
typedef struct { u64 k1, k2, val; } tent;
typedef struct { chunk *head[NPART], *tail[NPART]; u8 *slab, *slabend; u32 *scratch; u32 scratchcap; u64 nrec, ndistinct, npkghit; char pad[64]; } rworker;
static rworker *rw;
static struct { tent *e; u32 mask; } T[NPART];
#define BLOOMBITS (1u << 23)
static u64 *bloom;
#define rankNoDup (1u << 31)
#define rankContent (1u << 30)
#define rankDef (1u << 29)
#define rankIface (1u << 28)
#define IFACE ((u64)rankIface << 32)
static inline u32 rank_of(const object *o, u32 j, const u8 *s) {
  u32 siz = s_siz(s), r = (siz < (1u << 28) - 1 ? siz : (1u << 28) - 1) | rankDef;
  if (s_flag2(s) & SymFlagUsedInIface) r |= rankIface;
  if (s_flag(s) & SymFlagDupok) return r;
  r |= rankNoDup;
  u32 dn; data(o, j, &dn);
  if (s_type(s) == K_STEXT || s_type(s) == K_STEXTFIPS || dn) r |= rankContent;
  return r;
}
static inline void *slab(rworker *w, size_t n) {
  if (w->slab + n > w->slabend) { w->slab = big(4 << 20); w->slabend = w->slab + (4 << 20); }
  void *p = w->slab; w->slab += n; return p;
}
static inline void nadd(rworker *w, nrec r) {
  u32 p = r.k.k1 >> 56; chunk *c = w->tail[p];
  if (!c || c->n == CHUNK) { chunk *d = slab(w, sizeof(chunk)); d->next = 0; d->n = 0; if (c) c->next = d; else w->head[p] = d; w->tail[p] = c = d; }
  c->r[c->n++] = r;
}
static void scan_names(int w, int ui) {
  unit *u = &units[ui]; object *o = u->o;
  u32 lo = u->lo > o->nplo ? u->lo : o->nplo;
  for (u32 j = lo; j < u->hi; j++) {
    const u8 *s = symrec(o, j); u32 gid = o->base + j, rk;
    if (j < o->reflo) {
      if (s_abi(s) == SymABIstatic || s_namelen(s) == 0) { canon[gid] = gid; continue; }
      rk = rank_of(o, j, s);
    } else rk = s_flag2(s) & SymFlagUsedInIface ? rankIface : 0;
    nadd(&rw[w], (nrec){namekey(s_name(o, s), s_namelen(s), s_abi(s)), (u64)rk << 32 | (u32)~gid});
  }
}
static inline u64 better(u64 a, u64 b) {      // the winner of two records of one name; the interface flag rides along
  u64 iface = (a | b) & IFACE; a &= ~IFACE; b &= ~IFACE;
  if ((a >> 62) == 3 && (b >> 62) == 3 && a != b) die("duplicated definition of a symbol (ids %u and %u)", ~(u32)a, ~(u32)b);
  return (a > b ? a : b) | iface;
}
static void build_part(int wi, int p) {
  rworker *w = &rw[wi]; u64 n = 0;
  for (int t = 0; t < nthreads; t++) for (chunk *c = rw[t].head[p]; c; c = c->next) n += c->n;
  u32 size = 64; while (size < 2 * n) size <<= 1;
  if (w->scratchcap < size) { free(w->scratch); w->scratch = malloc(4 * (size_t)size + sizeof(tent) * (size_t)size / 2 + 64); w->scratchcap = size; }
  u32 *idx = w->scratch; tent *ent = (tent *)(idx + size); u32 nd = 0, mask = size - 1;
  memset(idx, 0, 4 * (size_t)size);
  for (int t = 0; t < nthreads; t++)
    for (chunk *c = rw[t].head[p]; c; c = c->next)
      for (u32 i = 0; i < c->n; i++) {
        nrec *r = &c->r[i];
        for (u32 h = (u32)(r->k.k1 >> 8) & mask;; h = (h + 1) & mask) {
          u32 e = idx[h];
          if (!e) { idx[h] = ++nd; ent[nd - 1] = (tent){r->k.k1, r->k.k2, r->val}; break; }
          if (ent[e - 1].k1 == r->k.k1 && ent[e - 1].k2 == r->k.k2) { ent[e - 1].val = better(ent[e - 1].val, r->val); break; }
        }
      }
  // the partition's table: the distinct names, at half load or less
  u32 tsize = 16; while (tsize < 2 * nd) tsize <<= 1;
  tent *te = slab(w, sizeof(tent) * (size_t)tsize); memset(te, 0, sizeof(tent) * (size_t)tsize);
  for (u32 i = 0; i < nd; i++) {
    for (u32 h = (u32)(ent[i].k1 >> 8) & (tsize - 1);; h = (h + 1) & (tsize - 1)) if (!te[h].k1) { te[h] = ent[i]; break; }
    u64 b1 = ent[i].k2 % BLOOMBITS, b2 = (ent[i].k2 >> 32) % BLOOMBITS;
    __atomic_fetch_or(&bloom[b1 >> 6], 1ull << (b1 & 63), __ATOMIC_RELAXED); __atomic_fetch_or(&bloom[b2 >> 6], 1ull << (b2 & 63), __ATOMIC_RELAXED);
  }
  T[p].e = te; T[p].mask = tsize - 1; w->nrec += n; w->ndistinct += nd;
}
static inline tent *t_find(key k) {
  u32 p = k.k1 >> 56; tent *te = T[p].e; u32 mask = T[p].mask;
  if (!te) return 0;
  for (u32 h = (u32)(k.k1 >> 8) & mask;; h = (h + 1) & mask) {
    if (te[h].k1 == 0) return 0;
    if (te[h].k1 == k.k1 && te[h].k2 == k.k2) return &te[h];
  }
}
static void scan_pkgdefs(int w, int ui) {      // package-indexed definitions: themselves, unless something refers to their name
  unit *u = &units[ui]; object *o = u->o;
  u32 hi = u->hi < o->ndef ? u->hi : o->ndef;
  for (u32 j = u->lo; j < hi; j++) {
    const u8 *s = symrec(o, j); u32 gid = o->base + j;
    if (s_flag2(s) & SymFlagUsedInIface) __atomic_fetch_or(&state[gid], stUsedInIface, __ATOMIC_RELAXED);
    if (s_abi(s) == SymABIstatic || s_namelen(s) == 0) continue;
    key k = namekey(s_name(o, s), s_namelen(s), s_abi(s));
    u64 b1 = k.k2 % BLOOMBITS, b2 = (k.k2 >> 32) % BLOOMBITS;
    if (!(bloom[b1 >> 6] >> (b1 & 63) & 1) || !(bloom[b2 >> 6] >> (b2 & 63) & 1)) continue;
    tent *e = t_find(k);
    if (!e) continue;
    u64 val = (u64)rank_of(o, j, s) << 32 | (u32)~gid, old = __atomic_load_n(&e->val, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&e->val, &old, better(old, val), 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    rw[w].npkghit++;
  }
}
static void finish_part(int wi, int p) {       // every record's slot learns its name's winner
  for (int t = 0; t < nthreads; t++)
    for (chunk *c = rw[t].head[p]; c; c = c->next)
      for (u32 i = 0; i < c->n; i++) {
        nrec *r = &c->r[i]; u32 gid = ~(u32)r->val;
        if (gid == ~0u) continue;                      // (a name the linker wants: not a slot)
        canon[gid] = ~(u32)t_find(r->k)->val;
      }
  tent *te = T[p].e;
  if (te) for (u32 h = 0; h <= T[p].mask; h++) {
    if (!te[h].k1) continue;
    u32 c = ~(u32)te[h].val; if (c == ~0u) continue;
    u8 f = (te[h].val >> 32 & rankDef ? 0 : stUndef) | (te[h].val & IFACE ? stUsedInIface : 0);
    if (f) __atomic_fetch_or(&state[c], f, __ATOMIC_RELAXED);
  }
}
static const char *linker_names[] = {"runtime.firstmoduledata", "runtime.lastmoduledatap", "runtime.runtime_inittasks", "main..inittask", "runtime..inittask",
  "runtime.unreachableMethod", "runtime.mapinitnoop", "runtime.deferreturn", "runtime.memProfileInternal", "runtime.disableMemoryProfiling", "main.main",
  "_rt0_amd64_linux", "runtime.buildVersion", "runtime.modinfo", "runtime.defaultGOROOT", "runtime.godebugDefault", 0};
static key strkey(const char *name, u16 abi) { size_t n = strlen(name); char buf[512]; memset(buf, 0, sizeof buf); memcpy(buf, name, n); return namekey((const u8 *)buf, n, abi); }
static u32 lookup(const char *name, u16 abi) {
  tent *e = t_find(strkey(name, abi)); if (!e) return 0;
  u32 c = ~(u32)e->val; return c == ~0u ? 0 : c;
}
static void resolve(void) {
  double t0 = now_ms();
  canon = big(4 * ((size_t)nslots + 1)); state = big((size_t)nslots + 1); bloom = big(BLOOMBITS / 8);
  rw = calloc(nthreads, sizeof(rworker));
  // names the linker itself looks up: package-indexed definitions of these must join by name
  for (int i = 0; i < NBUILTIN; i++) nadd(&rw[0], (nrec){strkey(builtin_names[i].name, builtin_names[i].abi), 0});
  for (const char **n = linker_names; *n; n++) for (int abi = 0; abi < 2; abi++) nadd(&rw[0], (nrec){strkey(*n, abi), 0});
  par(nunits, scan_names);
  double t1 = now_ms();
  par(NPART, build_part);
  double t2 = now_ms();
  par(nunits, scan_pkgdefs);
  double t3 = now_ms();
  par(NPART, finish_part);
  for (int i = 0; i < NBUILTIN; i++) { u32 g = lookup(builtin_names[i].name, builtin_names[i].abi); builtins[i] = is_def(g) ? g : 0; }
  double t4 = now_ms();
  u64 nrec = 0, nd = 0, hits = 0; for (int t = 0; t < nthreads; t++) { nrec += rw[t].nrec; nd += rw[t].ndistinct; hits += rw[t].npkghit; }
  fprintf(stderr, "resolve: %llu records by name, %llu distinct names, %llu package-indexed definitions joined\n", (unsigned long long)nrec, (unsigned long long)nd, (unsigned long long)hits);
  fprintf(stderr, "resolve ms: scan names %.2f, build %.2f, pkgdefs %.2f, finish %.2f = %.2f\n", t1 - t0, t2 - t1, t3 - t2, t4 - t3, t4 - t0);
}

// ---- the lazy join of content-addressed symbols: a lock-free table of the classes the flood has reached
typedef struct { u64 k1; u64 k2; u32 first; u32 min; } hent;       // first: the member that claimed the entry (0 until the entry is complete)
static hent *H; static u64 hmask;
static inline u64 mix64(u64 h) { h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 27; h *= 0x94D049BB133111EBull; return h ^ h >> 31; }
static inline void hkey(const object *o, u32 j, u64 *k1, u64 *k2) {
  if (j >= o->hlo) { const u8 *h = o->hash + 16 * (size_t)(j - o->hlo); *k1 = rd64(h); *k2 = rd64(h + 8); }
  else { u64 h = rd64(o->hash64 + 8 * (size_t)(j - o->h64lo)); *k1 = mix64(h ^ 0x5851F42D4C957F2Dull); *k2 = h; }
  if (*k1 == 0) *k1 = 1;
}
// intern: slot gid (content-addressed) has been reached; returns its class entry
static inline hent *intern(const object *o, u32 j, u32 gid) {
  u64 k1, k2; hkey(o, j, &k1, &k2);
  for (u64 h = k1 & hmask;; h = (h + 1) & hmask) {
    hent *e = &H[h]; u64 cur = __atomic_load_n(&e->k1, __ATOMIC_ACQUIRE);
    if (cur == 0) {
      if (__atomic_compare_exchange_n(&e->k1, &cur, k1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        e->k2 = k2; __atomic_store_n(&e->min, gid, __ATOMIC_RELAXED); __atomic_store_n(&e->first, gid, __ATOMIC_RELEASE);
        return e;
      }
    }
    if (cur != k1) continue;
    while (__atomic_load_n(&e->first, __ATOMIC_ACQUIRE) == 0) _mm_pause();      // its owner is filling it in
    if (e->k2 != k2) continue;
    u32 m = __atomic_load_n(&e->min, __ATOMIC_RELAXED);
    while (gid < m && !__atomic_compare_exchange_n(&e->min, &m, gid, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return e;
  }
}
static inline int is_hashed(const object *o, u32 j) { return j >= o->h64lo && j < o->nplo; }

// ---- reachability: cmd/link's deadcode, as a flood by levels
typedef struct { const u8 *name; u32 nlen; u32 typ; } msig;
typedef struct { msig s; u32 src, r; } mref;
typedef struct { mref *v; u32 n, cap; } mvec;
typedef struct { msig *v; u32 n, cap; } svec;
typedef struct { vec next; vec hreached; mvec methods; svec ifacem; svec generic; int reflect; vec tmp; u64 nrel, nsym, naux; char pad[64]; } fworker;
static fworker *fw;
static inline void mark(fworker *w, u32 c) {
  if (c == 0) return;
  u8 old = __atomic_fetch_or(&state[c], stLive, __ATOMIC_RELAXED);
  if (!(old & (stLive | stUndef))) push(&w->next, c);
}
static inline void set_iface(fworker *w, u32 c) {
  u8 old = __atomic_fetch_or(&state[c], stUsedInIface, __ATOMIC_RELAXED);
  if (!(old & stUsedInIface) && (old & stLive)) push(&w->next, c);
}
static inline const u8 *reloc_at(const u8 *rel, u32 n, i32 off) { for (u32 i = 0; i < n; i++) if (r_off(rel + i * RelocSize) == off) return rel + i * RelocSize; return 0; }
static inline const u8 *data_of(u32 c, u32 *n) { *n = 0; if (!is_def(c)) return 0; u32 j; object *o = obj_of(c, &j); if (j >= o->nalldef) return 0; return data(o, j, n); }
// name_data: a type:.namedata symbol is a flag byte, a varint length, the name
static msig name_data(const u8 *d, u32 n) {
  msig s = {0, 0, 0}; if (n < 2) return s;
  u32 len = 0, k = 1, sh = 0; for (;; k++) { len |= (u32)(d[k] & 0x7f) << sh; sh += 7; if (!(d[k] & 0x80)) { k++; break; } }
  s.name = d + k; s.nlen = len; return s;
}
static msig name_at(const object *o, const u8 *rel, u32 nrel, i32 off) {
  const u8 *r = reloc_at(rel, nrel, off); msig z = {0, 0, 0}; if (!r) return z;
  u32 n; const u8 *d = data_of(canat(o, r + 15), &n); return d ? name_data(d, n) : z;
}
static void process(fworker *w, u32 c) {
  u32 j; object *o = obj_of(c, &j); const u8 *s = symrec(o, j);
  w->nsym++;
  if (is_hashed(o, j)) push(&w->hreached, c);
  if (s_flag(s) & SymFlagReflectMethod) w->reflect = 1;
  int is_type = s_flag(s) & SymFlagGoType, uii = 0;
  if (is_type && (__atomic_load_n(&state[c], __ATOMIC_RELAXED) & stUsedInIface)) {
    uii = 1;
    if (__atomic_fetch_or(&state[c], stIfaceDone, __ATOMIC_RELAXED) & stIfaceDone) return;
  }
  u32 nrel; const u8 *rel = relocs(o, j, &nrel);
  w->nrel += nrel; w->tmp.n = 0;
  for (u32 i = 0; i < nrel; i++) {
    const u8 *rl = rel + i * RelocSize; u16 t = r_type(rl);
    if (t & R_WEAK) continue;
    switch (t) {
    case R_METHODOFF:
      if (i + 2 >= nrel) die("expect three consecutive R_METHODOFF relocs");
      if (uii) { push(&w->tmp, i); u32 rs = canat(o, rl + 15); if (rs) set_iface(w, rs); }
      i += 2; continue;
    case R_USETYPE: case R_INITORDER: continue;
    case R_USEIFACE: {
      u32 rs = canat(o, rl + 15); if (!is_def(rs)) continue;
      u32 j2; object *o2 = obj_of(rs, &j2);
      if (s_flag2(symrec(o2, j2)) & SymFlagItab) {
        u32 n2; const u8 *rel2 = relocs(o2, j2, &n2); const u8 *rl2 = reloc_at(rel2, n2, 8);
        if (!rl2) continue;
        rs = canat(o2, rl2 + 15); if (!is_def(rs)) continue;
      }
      set_iface(w, rs); continue; }
    case R_USEIFACEMETHOD: {
      u32 rs = canat(o, rl + 15);
      if (is_def(rs)) {
        u32 j2; object *o2 = obj_of(rs, &j2); u32 n2; const u8 *rel2 = relocs(o2, j2, &n2); i32 off = (i32)r_add(rl);
        msig m = name_at(o2, rel2, n2, off); const u8 *rl2 = reloc_at(rel2, n2, off + 4);
        if (rl2) m.typ = canat(o2, rl2 + 15);
        if (w->ifacem.n == w->ifacem.cap) { w->ifacem.cap = w->ifacem.cap ? 2 * w->ifacem.cap : 256; w->ifacem.v = realloc(w->ifacem.v, w->ifacem.cap * sizeof(msig)); }
        w->ifacem.v[w->ifacem.n++] = m;
      }
      continue; }
    case R_USENAMEDMETHOD: {
      u32 n; const u8 *d = data_of(canat(o, rl + 15), &n); msig m = {d, n, 0};
      if (w->generic.n == w->generic.cap) { w->generic.cap = w->generic.cap ? 2 * w->generic.cap : 64; w->generic.v = realloc(w->generic.v, w->generic.cap * sizeof(msig)); }
      w->generic.v[w->generic.n++] = m; continue; }
    }
    u32 rs = canat(o, rl + 15);
    if (rs == 0) continue;
    if (uii && is_def(rs) && !(__atomic_load_n(&state[rs], __ATOMIC_RELAXED) & stUsedInIface)) {
      u32 j2; object *o2 = obj_of(rs, &j2);
      if (s_flag(symrec(o2, j2)) & SymFlagGoType) {
        u8 old = __atomic_fetch_or(&state[rs], stUsedInIface | stLive, __ATOMIC_RELAXED);
        if (!(old & stUsedInIface) || !(old & stLive)) push(&w->next, rs);
        continue;
      }
    }
    mark(w, rs);
  }
  u8 kind = s_type(s);
  if (kind == K_STEXT || kind == K_STEXTFIPS) {
    u32 na; const u8 *ax = auxs(o, j, &na); u32 ninl = 0; w->naux += na;
    for (u32 i = 0; i < na; i++) if (ax[i * AuxSize] == AuxFuncInfo) {
      u32 n; const u8 *fi = data_of(canat(o, ax + i * AuxSize + 1), &n);
      if (!fi) break;
      u32 nfile = rd32(fi + 16); ninl = rd32(fi + 20 + 4 * nfile); const u8 *tree = fi + 24 + 4 * nfile;
      for (u32 k = 0; k < ninl; k++) { u32 callee = canat(o, tree + 24 * k + 12); if (callee) __atomic_fetch_or(&state[callee], stNameNeeded, __ATOMIC_RELAXED); }
      break;
    }
    for (u32 i = 0; i < na; i++) {
      const u8 *a = ax + i * AuxSize;
      switch (a[0]) {
      case AuxFuncdata: { u32 g = canat(o, a + 1); if (g) { __atomic_fetch_or(&state[g], stFuncdata, __ATOMIC_RELAXED); mark(w, g); } break; }
      case AuxPcinline: if (ninl == 0) break; /* fallthrough */
      case AuxPcsp: case AuxPcfile: case AuxPcline: case AuxPcdata: { u32 g = canat(o, a + 1); if (g) { u8 old = __atomic_fetch_or(&state[g], stPC, __ATOMIC_RELAXED); if (!(old & stPC)) push(&w->hreached, g); } break; }
      }
    }
  }
  if (w->tmp.n) {
    u32 dn; const u8 *p = data(o, j, &dn);
    if (!(p[TFlagOff] & TFlagUncommon)) die("methods on a type with no uncommon type");
    u32 off = rtypeSize[p[23] & 31], mcount = rd16(p + off + 4), moff = rd32(p + off + 8);
    if (mcount != w->tmp.n) die("%u method relocations for %u methods", w->tmp.n, mcount);
    for (u32 i = 0; i < w->tmp.n; i++) {
      u32 ri = w->tmp.v[i]; i32 at = (i32)(off + moff + 16 * i); msig m;
      if (ri > 0 && r_off(rel + (ri - 1) * RelocSize) == at) { u32 n; const u8 *d = data_of(canat(o, rel + (ri - 1) * RelocSize + 15), &n); m = name_data(d, n); }
      else m = name_at(o, rel, nrel, at);
      m.typ = canat(o, rel + ri * RelocSize + 15);
      if (w->methods.n == w->methods.cap) { w->methods.cap = w->methods.cap ? 2 * w->methods.cap : 1024; w->methods.v = realloc(w->methods.v, w->methods.cap * sizeof(mref)); }
      w->methods.v[w->methods.n++] = (mref){m, c, ri};
    }
  }
}
static u32 *frontier; static u32 nfrontier, capfrontier;
#define GRAIN 64
static void flood_chunk(int w, int i) { u32 hi = (i + 1) * GRAIN < nfrontier ? (i + 1) * GRAIN : nfrontier; for (u32 k = i * GRAIN; k < hi; k++) process(&fw[w], frontier[k]); }
// sets of method signatures: open addressing on a 64-bit hash of (name, type)
typedef struct { u64 *v; u64 mask, n; } set;
static void set_add(set *s, u64 k) {
  if (!k) k = 1;
  if (4 * (s->n + 1) > 3 * (s->mask + 1)) { set o = *s; s->mask = s->mask ? 2 * s->mask + 1 : 1023; s->v = calloc(s->mask + 1, 8); s->n = 0; for (u64 i = 0; o.v && i <= o.mask; i++) if (o.v[i]) set_add(s, o.v[i]); free(o.v); }
  for (u64 h = k & s->mask;; h = (h + 1) & s->mask) { if (!s->v[h]) { s->v[h] = k; s->n++; return; } if (s->v[h] == k) return; }
}
static int set_has(const set *s, u64 k) { if (!s->v) return 0; if (!k) k = 1; for (u64 h = k & s->mask;; h = (h + 1) & s->mask) { if (!s->v[h]) return 0; if (s->v[h] == k) return 1; } }
static u64 sig_key(msig m, int with_type) {
  u8 buf[32]; const u8 *p = m.name; if (m.nlen < 16) { memset(buf, 0, 32); if (m.nlen) memcpy(buf, m.name, m.nlen); p = buf; }
  key k = namekey(p, m.nlen, 0); return k.k1 ^ (with_type ? mix64(m.typ + 1) : 0);
}
static set iface_method, generic_method; static mref *markable; static u32 nmarkable, capmarkable; static u8 *keep; static int reflect_seen;
static int is_exported(msig m) {      // unicode.IsUpper of the first rune (ASCII and Latin-1/Greek/Cyrillic capitals; rare beyond ASCII)
  if (!m.nlen) return 0;
  u8 c = m.name[0]; if (c < 0x80) return c >= 'A' && c <= 'Z';
  u32 r = 0; if ((c & 0xe0) == 0xc0 && m.nlen >= 2) r = (c & 0x1f) << 6 | (m.name[1] & 0x3f);
  return (r >= 0xc0 && r <= 0xde && r != 0xd7) || (r >= 0x391 && r <= 0x3a9) || (r >= 0x410 && r <= 0x42f);
}
static void method_chunk(int w, int i) {
  u32 hi = (i + 1) * 1024 < nmarkable ? (i + 1) * 1024 : nmarkable;
  for (u32 k = i * 1024; k < hi; k++) {
    mref *m = &markable[k];
    if ((reflect_seen && is_exported(m->s)) || set_has(&iface_method, sig_key(m->s, 1)) || set_has(&generic_method, sig_key(m->s, 0))) {
      u32 j; object *o = obj_of(m->src, &j); u32 n; const u8 *rel = relocs(o, j, &n);
      for (u32 d = 0; d < 3; d++) { u32 g = canat(o, rel + (m->r + d) * RelocSize + 15); if (g) mark(&fw[w], g); }
      keep[k] = 0;
    } else keep[k] = 1;
  }
}
static void inittask_roots(const char *root) {     // every inittask reachable over R_INITORDER edges that has something to run
  u32 r0 = lookup(root, 0); if (!is_def(r0)) return;
  vec q = {0}, seen = {0}; push(&q, r0); push(&seen, r0);
  while (q.n) {
    u32 x = q.v[--q.n], j; if (!is_def(x)) continue;
    object *o = obj_of(x, &j); u32 n; const u8 *rel = relocs(o, j, &n);
    if (s_siz(symrec(o, j)) > 8) mark(&fw[0], x);
    for (u32 i = 0; i < n; i++) if (r_type(rel + i * RelocSize) == R_INITORDER) {
      u32 s = canat(o, rel + i * RelocSize + 15); int dup = 0;
      for (u32 k = 0; k < seen.n; k++) if (seen.v[k] == s) { dup = 1; break; }
      if (!dup) { push(&seen, s); push(&q, s); }
    }
  }
}
static u32 nclasses;
static void finish_hashed(int w, int t) {        // every reached content-addressed slot learns its class's representative
  vec *v = &fw[t].hreached;
  for (u32 i = 0; i < v->n; i++) {
    u32 g = v->v[i], j; object *o = obj_of(g, &j); hent *e = intern(o, j, g);
    canon[g] = e->min;
    if (e->min != g) __atomic_fetch_or(&state[g], stDup, __ATOMIC_RELAXED);
  }
}
static void intern_chunk(int w, int t) { vec *v = &fw[t].hreached; for (u32 i = 0; i < v->n; i++) { u32 g = v->v[i], j; object *o = obj_of(g, &j); if (is_hashed(o, j)) intern(o, j, g); } }
static void deadcode(void) {
  double t0 = now_ms();
  fw = calloc(nthreads, sizeof(fworker));
  fworker *w0 = &fw[0];
  mark(w0, lookup("_rt0_amd64_linux", 0)); mark(w0, lookup("_rt0_amd64_linux", 1));
  mark(w0, lookup("runtime.unreachableMethod", 0)); mark(w0, lookup("runtime.unreachableMethod", 1));
  mark(w0, lookup("runtime.mapinitnoop", 1)); mark(w0, lookup("runtime.firstmoduledata", 0));
  inittask_roots("main..inittask");
  if (lookup("runtime.runtime_inittasks", 0)) inittask_roots("runtime..inittask");
  int levels = 0, rounds = 0;
  for (;;) {
    rounds++;
    for (;;) {
      nfrontier = 0;
      for (int t = 0; t < nthreads; t++) {
        vec *v = &fw[t].next;
        if (nfrontier + v->n > capfrontier) { capfrontier = 2 * (nfrontier + v->n) + 1024; frontier = realloc(frontier, 4 * (size_t)capfrontier); }
        memcpy(frontier + nfrontier, v->v, 4 * (size_t)v->n); nfrontier += v->n; v->n = 0;
      }
      if (!nfrontier) break;
      levels++;
      if (nfrontier < 4 * GRAIN) for (u32 k = 0; k < nfrontier; k++) process(w0, frontier[k]);
      else par((nfrontier + GRAIN - 1) / GRAIN, flood_chunk);
    }
    for (int t = 0; t < nthreads; t++) {
      fworker *w = &fw[t];
      for (u32 i = 0; i < w->ifacem.n; i++) set_add(&iface_method, sig_key(w->ifacem.v[i], 1));
      for (u32 i = 0; i < w->generic.n; i++) set_add(&generic_method, sig_key(w->generic.v[i], 0));
      reflect_seen |= w->reflect;
      if (nmarkable + w->methods.n > capmarkable) { capmarkable = 2 * (nmarkable + w->methods.n); markable = realloc(markable, capmarkable * sizeof(mref)); keep = realloc(keep, capmarkable); }
      memcpy(markable + nmarkable, w->methods.v, w->methods.n * sizeof(mref)); nmarkable += w->methods.n;
      w->ifacem.n = w->generic.n = w->methods.n = 0;
    }
    par((nmarkable + 1023) / 1024, method_chunk);
    u32 k2 = 0; for (u32 k = 0; k < nmarkable; k++) if (keep[k]) markable[k2++] = markable[k];
    nmarkable = k2;
    int more = 0; for (int t = 0; t < nthreads; t++) more |= fw[t].next.n != 0;
    if (!more) break;
  }
  double t1 = now_ms();
  // the classes of the reached content-addressed slots
  u64 nh = 0; for (int t = 0; t < nthreads; t++) nh += fw[t].hreached.n;
  hmask = 1024; while (hmask < 2 * nh) hmask <<= 1;
  H = big(hmask * sizeof(hent)); hmask--;
  par(nthreads, intern_chunk);
  par(nthreads, finish_hashed);
  double t2 = now_ms();
  u64 nrel = 0, nsym = 0, naux = 0; for (int t = 0; t < nthreads; t++) { nrel += fw[t].nrel; nsym += fw[t].nsym; naux += fw[t].naux; }
  fprintf(stderr, "deadcode: %d levels in %d rounds, %u methods pruned, reflection %d; %llu symbols flooded, %llu relocations and %llu aux records read; "
          "%llu content-addressed slots reached\n", levels, rounds, nmarkable, reflect_seen, (unsigned long long)nsym, (unsigned long long)nrel, (unsigned long long)naux, (unsigned long long)nh);
  fprintf(stderr, "deadcode ms: flood %.2f, classes of content-addressed slots %.2f\n", t1 - t0, t2 - t1);
}

// ---- -dumplive: the names of the live canonical symbols, sorted (what fclink -dumplive prints)
typedef struct { const u8 *p; u32 n; } str;
static int cmpstr(const void *a, const void *b) { const str *x = a, *y = b; u32 n = x->n < y->n ? x->n : y->n; int c = memcmp(x->p, y->p, n); return c ? c : (int)x->n - (int)y->n; }
static void dumplive(const char *file) {
  str *v = malloc(sizeof(str) * 2000000); u32 n = 0; u32 ndup = 0;
  for (int i = 0; i < nobjs; i++) {
    object *o = objs[i];
    for (u32 j = 0; j < o->total; j++) {
      u32 g = o->base + j; u8 st = state[g];
      if (!(st & stLive)) continue;
      if (st & stDup) { ndup++; continue; }
      if (j >= o->nplo && canon[g] != g) continue;
      const u8 *s = symrec(o, j); v[n++] = (str){s_name(o, s), s_namelen(s)};
    }
  }
  qsort(v, n, sizeof(str), cmpstr);
  FILE *f = fopen(file, "w"); for (u32 i = 0; i < n; i++) { fwrite(v[i].p, 1, v[i].n, f); fputc('\n', f); } fclose(f);
  fprintf(stderr, "dumplive: %u live symbols (%u live duplicates of content-addressed symbols folded)\n", n, ndup);
}

int main(int argc, char **argv) {
  const char *cfg = 0, *dump = 0, *mainfile = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-importcfg")) cfg = argv[++i];
    else if (!strcmp(argv[i], "-j")) nthreads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-dumplive")) dump = argv[++i];
    else mainfile = argv[i];
  }
  if (!cfg) die("usage: lazy -importcfg importcfg.link [-j N] [-dumplive file] [main.a]");
  double t0 = now_ms(), s0 = cpu_ms(1), u0 = cpu_ms(0);
  pool_start();
  load(cfg, mainfile);
  double t1 = now_ms(), s1 = cpu_ms(1), u1 = cpu_ms(0);
  resolve();
  double t2 = now_ms(), s2 = cpu_ms(1), u2 = cpu_ms(0);
  deadcode();
  double t3 = now_ms(), s3 = cpu_ms(1), u3 = cpu_ms(0);
  printf("%-9s %8s %8s %8s\n", "phase", "wall ms", "user ms", "sys ms");
  printf("%-9s %8.2f %8.2f %8.2f  %d files, %d objects, %u slots\n", "load", t1 - t0, u1 - u0, s1 - s0, nlibs, nobjs, nslots);
  printf("%-9s %8.2f %8.2f %8.2f\n", "resolve", t2 - t1, u2 - u1, s2 - s1);
  printf("%-9s %8.2f %8.2f %8.2f\n", "deadcode", t3 - t2, u3 - u2, s3 - s2);
  printf("%-9s %8.2f %8.2f %8.2f  %d threads\n", "total", t3 - t0, u3 - u0, s3 - s0, nthreads);
  if (dump) dumplive(dump);
  fflush(stdout); _exit(0);
}
