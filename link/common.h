// common.h: types, helpers and globals shared by every phase of hlink. hlink.c includes this and then the
// phase files, so the whole linker is one translation unit.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#define HLINK_BUILTINS
#define HLINK_RELOCNAMES
#include "gotables.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define UNUSED __attribute__((unused))

// ---- unaligned little-endian loads and stores (the object records are packed)
static inline u16 ld16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 ld32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 ld64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline void st16(void *p, u16 v) { memcpy(p, &v, 2); }
static inline void st32(void *p, u32 v) { memcpy(p, &v, 4); }
static inline void st64(void *p, u64 v) { memcpy(p, &v, 8); }

static inline u32 rnd(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// ---- strings and byte ranges that alias the inputs: never copied
typedef struct {
	const char *p;
	u32 n;
} str;

typedef struct {
	const u8 *p;
	u32 n; // n == 0 is Go's nil slice
} bytes;

static inline str cstr(const char *c) { return (str){c, (u32)strlen(c)}; }
static inline bool streq(str a, str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static inline bool hasPrefix(str s, const char *pre) {
	size_t n = strlen(pre);
	return s.n >= n && memcmp(s.p, pre, n) == 0;
}
// Go's string order: bytewise, then by length
static inline int strcmpGo(str a, str b) {
	u32 n = MIN(a.n, b.n);
	int c = n ? memcmp(a.p, b.p, n) : 0;
	if (c != 0) return c;
	return a.n < b.n ? -1 : a.n > b.n;
}
static char *strdupStr(str s) {
	char *c = malloc(s.n + 1);
	memcpy(c, s.p, s.n);
	c[s.n] = 0;
	return c;
}

// ---- growable vectors, for per-thread and per-unit scratch (never per symbol)
#define VEC(T) struct { T *a; size_t n, cap; }
#define VPUSH(v, x)                                                                                 \
	do {                                                                                            \
		if ((v).n == (v).cap) {                                                                     \
			(v).cap = (v).cap ? 2 * (v).cap : 64;                                                   \
			(v).a = realloc((v).a, (v).cap * sizeof *(v).a);                                        \
			if (!(v).a) { fprintf(stderr, "hlink: out of memory\n"); _exit(2); }                    \
		}                                                                                           \
		(v).a[(v).n++] = (x);                                                                       \
	} while (0)
#define VRESERVE(v, k)                                                                              \
	do {                                                                                            \
		if ((v).cap < (k)) {                                                                        \
			(v).cap = (k);                                                                          \
			(v).a = realloc((v).a, (v).cap * sizeof *(v).a);                                        \
			if (!(v).a) { fprintf(stderr, "hlink: out of memory\n"); _exit(2); }                    \
		}                                                                                           \
	} while (0)

typedef VEC(u32) vecu32;
typedef VEC(u8) vecu8;

// ---- flags
static const char *flagOut = "a.out", *flagImportcfg = "", *flagBuildid = "", *flagDumpLive = "", *flagDumpMap = "";
static const char *flagEntry = "_rt0_amd64_linux";
static bool flagStats, flagNoDCE;
static int flagJ;

static int nthreads;

// ---- errors (main.c)
static void errorf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fatalf(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void exitIfErrors(void);
static void lap(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- the thread pool (main.c): fn(ctx, worker, i) for i in [0,n), on all threads, in no defined order
typedef void (*parfn)(void *ctx, int w, long i);
static void par(long n, parfn fn, void *ctx);

// ---- big zeroed arrays come from mmap
static void *bigalloc(size_t n) {
	if (n == 0) n = 1;
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) fatalf("mmap %zu bytes: %s", n, strerror(errno));
	return p;
}

// ---- objects (load.c)
// An object is one Go object file: a whole bare object, or one member of an archive. Its symbol slots are, in
// order: definitions referenced by package index (ndef), content-addressed definitions with a 64-bit hash
// (nh64) and with a longer hash (nh), definitions referenced by name (nnonpkg), and references by name to
// symbols defined elsewhere (nref). Slot j has the global id base+j.
typedef struct object {
	int idx;
	str pkg;
	const char *file;
	const u8 *b; // the object file proper (after the "go object" text header)
	u32 off[NBlk];
	u32 flags;
	u32 base;
	u32 ndef, nh64, nh, nnonpkg, nref;
	u32 nalldef, total;
	u32 npkg;
	u32 *pkgBase; // per package index of this object: base of that package's primary object
	bool fromAsm;
	u32 nfunc; // live functions
	u32 cuOff; // index of this object's first file in cutab
} object;

#define badBase 0xFFFFFFFFu

// A unit is a run of slots of one object, the grain of every parallel pass.
enum {
	bNone,
	bText,
	bTextFIPS,
	bRodata,
	bRodataFIPS,
	bGofunc,
	bTypelink, // laid out one by one in sorted order, not by unit
	bType,
	bItab,
	bNoptrdata,
	bNoptrdataFIPS,
	bData,
	bDataFIPS,
	bBSS,
	bNoptrBSS,
	bCov,
	nBuckets,
};
static const char *const bucketName[nBuckets] = {"none", "text", "textfips", "rodata", "rodatafips", "gofunc", "typelink",
	"type", "itab", "noptrdata", "noptrdatafips", "data", "datafips", "bss", "noptrbss", "covctrs"};

#define nClasses 13 // alignment 1<<0 .. 1<<12
#define stClassShft 13
#define imageBase 0x400000u
#define headerSize 0x1000u
#define funcAlign 32u
#define maxAlign 32u
#define funcSize (11 * 4) // runtime._func
#define minFunc ABI_MINFUNC

typedef struct unit {
	object *o;
	u32 lo, hi; // slots [lo,hi) of o
	// layout: bytes this unit's live symbols need per (bucket, alignment class), then where they go
	u32 csize[nBuckets][nClasses];
	u32 cbase[nBuckets][nClasses];
	u32 nfunc[2]; // live functions (ordinary, FIPS), and the index of the first of each
	u32 funcBase[2];
	u32 nfuncCl[2][nClasses]; // live functions per alignment class: the function table is in address order
	u32 ftabSize, ftabBase; // bytes of _func structs with their pcdata and funcdata offsets
	u32 nameSize, nameBase; // bytes of function names
	u32 pcSize, pcBase;     // bytes of pc-value tables this unit owns
	u32 inlSize, inlBase;   // bytes of inline trees
	vecu32 typelinks;       // live types that go in the sorted part of the type section
	u64 nrel;
} unit;

#define unitSlots 8192u
#define pageShift 9

static object **objs;
static int nobjs;
static unit *units;
static long nunits;
static u32 nslots; // ids 1..nslots are object slots; ids above are symbols the linker makes
static u32 *objPage;
static object *mainObj;

// string variables the linker sets: runtime.buildVersion, runtime.modinfo, -X (later wins)
typedef struct {
	char *name;
	str val; // (may hold NUL bytes: runtime.modinfo does)
} strdef;
static VEC(strdef) strdata;
static void setStrdata(const char *name, str val) {
	char *v = malloc(val.n + 1);
	memcpy(v, val.p, val.n);
	v[val.n] = 0;
	for (size_t i = 0; i < strdata.n; i++)
		if (strcmp(strdata.a[i].name, name) == 0) {
			strdata.a[i].val = (str){v, val.n};
			return;
		}
	strdef d = {strdup(name), {v, val.n}};
	VPUSH(strdata, d);
}
static str getStrdata(const char *name) {
	for (size_t i = 0; i < strdata.n; i++)
		if (strcmp(strdata.a[i].name, name) == 0) return strdata.a[i].val;
	return (str){"", 0};
}

// ---- object record accessors: overlays on the mapped bytes
static inline const u8 *osym(const object *o, u32 j) { return o->b + o->off[BlkSymdef] + j * SymSize; }
static inline u32 symNameLen(const u8 *s) { return ld32(s); }
static inline str symNameOf(const object *o, const u8 *s) { return (str){(const char *)o->b + ld32(s + 4), ld32(s)}; }
static inline u16 symABI(const u8 *s) { return ld16(s + 8); }
static inline u8 symType(const u8 *s) { return s[10]; }
static inline u8 symFlag(const u8 *s) { return s[11]; }
static inline u8 symFlag2(const u8 *s) { return s[12]; }
static inline u32 symSiz(const u8 *s) { return ld32(s + 13); }
static inline u32 symAlignField(const u8 *s) { return ld32(s + 17); }
static inline bool symDupok(const u8 *s) { return symFlag(s) & SymFlagDupok; }
static inline bool symTypelink(const u8 *s) { return symFlag(s) & SymFlagTypelink; }
static inline bool symReflectMethod(const u8 *s) { return symFlag(s) & SymFlagReflectMethod; }
static inline bool symIsGoType(const u8 *s) { return symFlag(s) & SymFlagGoType; }
static inline bool symUsedInIface(const u8 *s) { return symFlag2(s) & SymFlagUsedInIface; }
static inline bool symIsItab(const u8 *s) { return symFlag2(s) & SymFlagItab; }
static inline bool kindIsText(u8 k) { return k == K_STEXT || k == K_STEXTFIPS; }

typedef const u8 *relocp; // goobj.Reloc: Off int32, Siz uint8, Type uint16, Add int64, Sym {PkgIdx, SymIdx}
static inline i32 rOff(relocp r) { return (i32)ld32(r); }
static inline u8 rSiz(relocp r) { return r[4]; }
static inline u16 rType(relocp r) { return ld16(r + 5); }
static inline i64 rAdd(relocp r) { return (i64)ld64(r + 7); }
static inline u32 rPkg(relocp r) { return ld32(r + 15); }
static inline u32 rIdx(relocp r) { return ld32(r + 19); }
#define RELOC(rs, i) ((rs) + (size_t)(i) * RelocSize)

typedef const u8 *auxp; // goobj.Aux: Type uint8, Sym {PkgIdx, SymIdx}
static inline u8 aType(auxp a) { return a[0]; }
static inline u32 aPkg(auxp a) { return ld32(a + 1); }
static inline u32 aIdx(auxp a) { return ld32(a + 5); }
#define AUX(as, i) ((as) + (size_t)(i) * AuxSize)

static inline relocp oRelocs(const object *o, u32 j, u32 *n) {
	const u8 *x = o->b + o->off[BlkRelocIdx] + j * 4;
	u32 a = ld32(x);
	*n = ld32(x + 4) - a;
	return o->b + o->off[BlkReloc] + (size_t)a * RelocSize;
}
static inline auxp oAuxs(const object *o, u32 j, u32 *n) {
	const u8 *x = o->b + o->off[BlkAuxIdx] + j * 4;
	u32 a = ld32(x);
	*n = ld32(x + 4) - a;
	return o->b + o->off[BlkAux] + (size_t)a * AuxSize;
}
static inline bytes oData(const object *o, u32 j) {
	const u8 *x = o->b + o->off[BlkDataIdx] + j * 4;
	u32 a = ld32(x), e = ld32(x + 4);
	return (bytes){o->b + o->off[BlkData] + a, e - a};
}
static inline u32 oDataSize(const object *o, u32 j) {
	const u8 *x = o->b + o->off[BlkDataIdx] + j * 4;
	return ld32(x + 4) - ld32(x);
}
static inline str oStringRef(const object *o, u32 off) {
	return (str){(const char *)o->b + ld32(o->b + off + 4), ld32(o->b + off)};
}
static inline str oPkg(const object *o, u32 i) { return oStringRef(o, o->off[BlkPkgIdx] + i * 8); }
static inline u32 oNFile(const object *o) { return (o->off[BlkFile + 1] - o->off[BlkFile]) / 8; }
static inline str oFile(const object *o, u32 i) { return oStringRef(o, o->off[BlkFile] + i * 8); }
static inline u32 oNRefFlags(const object *o) { return (o->off[BlkRefFlags + 1] - o->off[BlkRefFlags]) / RefFlagsSize; }

// objOf returns the object that holds slot gid and sets *j to the slot's index in it.
static inline object *objOf(u32 gid, u32 *j) {
	u32 k = objPage[gid >> pageShift];
	for (;;) {
		object *o = objs[k];
		if (gid - o->base < o->total) {
			*j = gid - o->base;
			return o;
		}
		k++;
	}
}

static u32 builtins[NBUILTIN];

// ref returns the id of the slot a symbol reference in object o names (not yet its canonical definition).
static void refError(const object *o, u32 pkg);
static inline u32 oref(const object *o, u32 pkg, u32 idx) {
	switch (pkg) {
	case PkgIdxInvalid: return 0;
	case PkgIdxSelf: return o->base + idx;
	case PkgIdxHashed64: return o->base + o->ndef + idx;
	case PkgIdxHashed: return o->base + o->ndef + o->nh64 + idx;
	case PkgIdxNone: return o->base + o->ndef + o->nh64 + o->nh + idx;
	case PkgIdxBuiltin: return builtins[idx];
	}
	if (pkg >= o->npkg) {
		refError(o, pkg);
		return 0;
	}
	u32 b = o->pkgBase[pkg];
	if (b == badBase) {
		refError(o, pkg);
		return 0;
	}
	return b + idx;
}
#define RREF(o, r) oref((o), rPkg(r), rIdx(r))
#define AREF(o, a) oref((o), aPkg(a), aIdx(a))

// ---- per-symbol state (resolve.c)
static u32 *canon; // per id: the id of the canonical definition (itself, for one); 0: none
static u32 *state; // per id: st* bits, written with atomic operations while passes run in parallel
static u32 *addr;  // per id: the address (pc-value tables: the offset in pctab); 0: not in the output
static u32 *aux;   // per id: for a function, the offset of its name in funcnametab
static u32 nids;   // ids in use: 1..nids-1 (object slots, then synthetic)
static u32 idCap;

enum {
	stLive = 1 << 0,        // reachable
	stUsedInIface = 1 << 1, // a type converted to an interface: its methods may be called dynamically
	stIfaceDone = 1 << 2,   // (deadcode) already flooded as such
	stPC = 1 << 3,          // a pc-value table of a live function: goes in pctab
	stFuncdata = 1 << 4,    // funcdata of a live function: goes in the go:func.* area
	stNameNeeded = 1 << 5,  // a function inlined into a live one: its name goes in funcnametab
	stOverride = 1 << 6,    // the linker writes this symbol's content itself
	stUndef = 1 << 7,       // a reference that stands for a symbol no object defines
	stBucketShift = 8,      // bits 8..12: the bucket; bits 13..16: the alignment class
};

static inline u32 atomicOr32(u32 *p, u32 v) { return __atomic_fetch_or(p, v, __ATOMIC_RELAXED); }
static inline u32 atomicLoad32(const u32 *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

// isDef: is canonical id c a definition in some object?
static inline bool isDef(u32 c) { return c != 0 && c <= nslots && (state[c] & stUndef) == 0; }
static inline bool isLive(u32 g) { return g != 0 && (state[g] & stLive) != 0; }

static inline bytes dataOf(u32 c) {
	if (!isDef(c)) return (bytes){0, 0};
	u32 j;
	object *o = objOf(c, &j);
	return oData(o, j);
}

static str *synthNames;
static u32 nsynth;
static str symName(u32 gid);
static u32 lookupName(str name, u16 abi);
static inline u32 lookupC(const char *name, u16 abi) { return lookupName(cstr(name), abi); }

// ---- the name hash: wyhash-style multiply-fold over 16-byte blocks
static inline u64 mum(u64 a, u64 b) {
	__uint128_t r = (__uint128_t)a * b;
	return (u64)r ^ (u64)(r >> 64);
}
static inline u64 hashBytes(const void *pv, size_t n) {
	const u8 *p = pv;
	const u64 P0 = 0xa0761d6478bd642full, P1 = 0xe7037ed1a0b428dbull, P2 = 0x8ebc6af09c88c6e3ull;
	u64 seed = P0 ^ (n * P2), a, b;
	size_t k = n;
	while (k > 16) {
		seed = mum(ld64(p) ^ P1, ld64(p + 8) ^ seed);
		p += 16;
		k -= 16;
	}
	if (k >= 8) {
		a = ld64(p);
		b = ld64(p + k - 8);
	} else if (k >= 4) {
		a = ld32(p);
		b = ld32(p + k - 4);
	} else if (k > 0) {
		a = ((u64)p[0] << 16) | ((u64)p[k >> 1] << 8) | p[k - 1];
		b = 0;
	} else {
		a = b = 0;
	}
	return mum(P1 ^ n, mum(a ^ P2, b ^ seed));
}
static inline u64 nameKey(str name, u16 abi) { return hashBytes(name.p, name.n) ^ (u64)abi * 0x9E3779B97F4A7C15ull; }
static inline u64 mix64(u64 h) {
	h ^= h >> 30;
	h *= 0xBF58476D1CE4E5B9ull;
	h ^= h >> 27;
	h *= 0x94D049BB133111EBull;
	return h ^ (h >> 31);
}

// uvarint: binary.Uvarint
static inline u64 uvarint(const u8 *p, u32 n, u32 *k) {
	u64 x = 0;
	unsigned s = 0;
	for (u32 i = 0; i < n && i < 10; i++) {
		u8 b = p[i];
		if (b < 0x80) {
			*k = i + 1;
			return x | (u64)b << s;
		}
		x |= (u64)(b & 0x7f) << s;
		s += 7;
	}
	*k = 0;
	return 0;
}

// ---- a string -> int map, built serially, read in parallel
typedef struct {
	str *keys;
	long *vals;
	u64 mask;
	size_t n;
} strmap;
static void smInit(strmap *m, size_t hint) {
	size_t sz = 64;
	while (sz < 2 * hint) sz <<= 1;
	m->keys = calloc(sz, sizeof(str));
	m->vals = calloc(sz, sizeof(long));
	m->mask = sz - 1;
	m->n = 0;
}
static long *smFind(const strmap *m, str k) {
	u64 h = hashBytes(k.p, k.n) & m->mask;
	for (;;) {
		if (m->keys[h].p == NULL) return NULL;
		if (streq(m->keys[h], k)) return &m->vals[h];
		h = (h + 1) & m->mask;
	}
}
static void smGrow(strmap *m);
// smPut: inserts k with v if absent; returns the slot of its value and whether it was added
static long *smPut(strmap *m, str k, long v, bool *added) {
	if (2 * (m->n + 1) > m->mask + 1) smGrow(m);
	static const char empty[1];
	if (k.p == NULL) k.p = empty;
	u64 h = hashBytes(k.p, k.n) & m->mask;
	for (;;) {
		if (m->keys[h].p == NULL) {
			m->keys[h] = k;
			m->vals[h] = v;
			m->n++;
			if (added) *added = true;
			return &m->vals[h];
		}
		if (streq(m->keys[h], k)) {
			if (added) *added = false;
			return &m->vals[h];
		}
		h = (h + 1) & m->mask;
	}
}
static void smGrow(strmap *m) {
	strmap o = *m;
	smInit(m, o.mask + 1);
	for (size_t i = 0; i <= o.mask; i++)
		if (o.keys[i].p) smPut(m, o.keys[i], o.vals[i], NULL);
	free(o.keys);
	free(o.vals);
}

// PathToPrefix (cmd/internal/objabi): escape a package path as the objects name it
static char *pathToPrefix(str s) {
	int slash = -1;
	for (u32 i = 0; i < s.n; i++)
		if (s.p[i] == '/') slash = (int)i;
	char *p = malloc(3 * s.n + 1), *q = p;
	static const char hex[] = "0123456789abcdef";
	for (u32 r = 0; r < s.n; r++) {
		u8 c = (u8)s.p[r];
		if (c <= ' ' || (c == '.' && (int)r > slash) || c == '%' || c == '"' || c >= 0x7F) {
			*q++ = '%';
			*q++ = hex[c >> 4];
			*q++ = hex[c & 0xF];
		} else {
			*q++ = (char)c;
		}
	}
	*q = 0;
	return p;
}
