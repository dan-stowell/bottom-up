// resolve.c: symbol resolution. Every slot of every object is resolved to the id of its canonical definition,
// by the stock loader's rules: package-indexed references are a table read; references by name join
// definitions by name and ABI; content-addressed symbols join by their hash.
//
// The joins are partitioned hash joins. One parallel pass over the units reads the symbol records in order and
// writes a record (128-bit key, rank, id) per symbol that takes part into one of the partitions chosen by the
// key's top bits; then each partition, small enough for a core's L2, is grouped by key on its own thread and
// every record's slot learns its group's winner. The winner is the maximum of a total order, so the result
// does not depend on which thread saw what first:
//
//	a definition beats a reference; one that may not be duplicated beats one that may (DUPOK);
//	then one with content (or code) beats one without (BSS);
//	then the larger one wins (the stock loader's rule for DUPOK and BSS symbols and for hashed ones);
//	then the one in the earlier object, at the lower index.
//
// A name nothing defines resolves to its first reference: that slot stands for the undefined symbol.
// Names are compared by a 128-bit hash and never by their bytes (comparing means a cache miss in some other
// object's string table per record; two different names with one hash would be linked as one symbol, at odds
// of about 1 in 10^26 per link, which is what content-addressed symbols already accept).
// Definitions referenced by package index take part in the join by name only if some object refers to their
// name (or the linker itself looks it up): after the by-name tables are built, one pass over those
// definitions probes them, with a Bloom filter of the tables' keys in front.

#define nParts 256        // partitions of the join by name (their tables are kept for lookups)
#define nHashParts 512    // partitions of the join by content hash
#define bloomBits (1u << 23)
#define rankNoDup (1u << 31)
#define rankContent (1u << 30)
#define rankDef (1u << 29)
#define rankIface (1u << 28) // (not part of the order: the UsedInIface flag rides along)
#define valIface ((u64)rankIface << 32)

typedef struct {
	u64 k1, k2;
} key128;

// nameKey128: 16 bytes a step. Reads up to 15 bytes past the name, so the name must have that much mapped
// memory after it: true of every name in an object (the string block is never last); names of the linker's
// own go through keyOfStr.
static inline key128 nameKey128(const u8 *p, size_t n, u16 abi) {
	u64 a = 0x9E3779B97F4A7C15ull ^ n, b = 0xD6E8FEB86659FD93ull ^ ((u64)abi * 0xA24BAED4963EE407ull);
	while (n > 16) {
		u64 x = ld64(p), y = ld64(p + 8);
		a = mum(x ^ a, y ^ 0xE7037ED1A0B428DBull);
		b = mum(x ^ 0x8EBC6AF09C88C6E3ull, y ^ b);
		p += 16;
		n -= 16;
	}
	u64 x = ld64(p), y = ld64(p + 8);
	if (n <= 8) {
		x = n == 8 ? x : x & ((1ull << (8 * n)) - 1);
		y = 0;
	} else if (n < 16) {
		y &= (1ull << (8 * (n - 8))) - 1;
	}
	a = mum(x ^ a, y ^ 0x589965CC75374CC3ull);
	b = mum(x ^ 0x1D8E4E27C47D124Full, y ^ b);
	key128 k = {mum(a, b ^ 0xEB44ACCAB455D165ull), mum(a ^ 0x4F1BBCDCBFA53E0Aull, b)};
	if (k.k1 == 0) k.k1 = 1; // 0 marks an empty table entry
	return k;
}
static key128 keyOfStr(str name, u16 abi) {
	u8 stack[512], *buf = name.n + 16 <= sizeof stack ? stack : malloc(name.n + 16);
	memcpy(buf, name.p, name.n);
	memset(buf + name.n, 0, 16);
	key128 k = nameKey128(buf, name.n, abi);
	if (buf != stack) free(buf);
	return k;
}

// A record of a join: the key, and val = rank << 32 | ~id, whose maximum over a group is the group's winner.
// (For content-addressed symbols the rank is the size.) Records live in chunks cut from a worker's slab and
// chained per partition, so filling a partition never copies and touches only the memory the records need.
typedef struct {
	u64 k1, k2, val;
} rec;
#define chunkRecs 84
typedef struct chunk {
	struct chunk *next;
	u64 n;
	rec r[chunkRecs];
} chunk;
typedef struct {
	chunk *head, *tail;
} plist;
typedef struct {
	u64 k1, k2, val;
} tent;

typedef struct {
	plist names[nParts];
	u8 *slab, *slabEnd;
	u32 *scratch; // a partition's grouping table: indices into ent
	tent *ent;
	u32 scratchCap;
	long nrec[3], npkgjoin;
	char pad[64];
} resolveWorker;

static resolveWorker *rws;
static struct {
	tent *e;
	u32 mask;
} nameTab[nParts];
static u64 *bloom;

// hugealloc: zeroed memory for the linker's own big arrays and slabs, on 2 MiB pages when the machine has
// some reserved (vm.nr_hugepages): a first touch of a 4 KiB page costs about 3 microseconds here, and the
// records of MinIO's joins alone are 25,000 of them.
static void *hugealloc(size_t n) {
	n = (n + (2u << 20) - 1) & ~(size_t)((2u << 20) - 1);
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (p == MAP_FAILED) p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) fatalf("mmap %zu bytes: %s", n, strerror(errno));
	return p;
}
static inline void *slabAlloc(resolveWorker *w, size_t n) {
	if (w->slab + n > w->slabEnd) {
		size_t blk = n > (4u << 20) ? n : (4u << 20);
		w->slab = hugealloc(blk);
		w->slabEnd = w->slab + blk;
	}
	void *p = w->slab;
	w->slab += n;
	return p;
}
static inline void recAdd(resolveWorker *w, plist *l, rec r) {
	chunk *c = l->tail;
	if (!c || c->n == chunkRecs) {
		chunk *d = slabAlloc(w, sizeof(chunk));
		d->next = NULL;
		d->n = 0;
		if (c) c->next = d;
		else l->head = d;
		l->tail = c = d;
	}
	c->r[c->n++] = r;
}

static inline tent *nameFind(key128 k) {
	u32 p = k.k1 >> 56;
	tent *te = nameTab[p].e;
	if (!te) return NULL;
	u32 mask = nameTab[p].mask;
	for (u32 h = (u32)(k.k1 >> 8) & mask;; h = (h + 1) & mask) {
		if (te[h].k1 == 0) return NULL;
		if (te[h].k1 == k.k1 && te[h].k2 == k.k2) return &te[h];
	}
}

// lookupName finds the canonical symbol of a name: a definition, or the reference that stands for an undefined
// symbol; 0 if no object mentions the name. Names that only package-indexed definitions carry are found only if
// they were announced with wantName before the join.
static u32 lookupName(str name, u16 abi) {
	tent *e = nameFind(keyOfStr(name, abi));
	if (!e) return 0;
	u32 c = ~(u32)e->val;
	return c == ~0u ? 0 : c; // (~0: only wanted, never seen)
}
static void wantName(str name, u16 abi) {
	key128 k = keyOfStr(name, abi);
	recAdd(&rws[0], &rws[0].names[k.k1 >> 56], (rec){k.k1, k.k2, 0});
}

// synth makes a symbol of the linker's own (nothing refers to it by name) and returns its id.
static u32 synth(const char *name) {
	if (nids >= idCap) fatalf("too many linker symbols");
	u32 g = nids++;
	synthNames[nsynth++] = cstr(strdup(name));
	canon[g] = g;
	return g;
}

static str symName(u32 gid) {
	if (gid == 0) return cstr("<nil>");
	if (gid > nslots) return synthNames[gid - nslots - 1];
	u32 j;
	object *o = objOf(gid, &j);
	return symNameOf(o, osym(o, j));
}

static u32 rankOf(const object *o, u32 j, const u8 *s) {
	u32 r = MIN(symSiz(s), (1u << 28) - 1) | rankDef;
	if (symUsedInIface(s)) r |= rankIface;
	if (symDupok(s)) return r;
	r |= rankNoDup;
	if (kindIsText(symType(s)) || oDataSize(o, j) != 0) r |= rankContent;
	return r;
}

// The names the linker itself looks up; definitions by package index of these names must join by name.
static const char *const linkerNames[] = {"runtime.firstmoduledata", "runtime.lastmoduledatap", "runtime.runtime_inittasks",
	"main..inittask", "runtime..inittask", "runtime.unreachableMethod", "runtime.mapinitnoop", "runtime.deferreturn",
	"runtime.memProfileInternal", "runtime.disableMemoryProfiling", "main.main"};

// sfOf: what the flood wants to know about a definition (its record s, its entry ri of the relocation index)
static inline u8 sfOf(const u8 *s, const u8 *ri) {
	u8 f = symFlag(s), k = symType(s);
	return (ld32(ri + 4) != ld32(ri) ? sfHasRel : 0) | (f & SymFlagGoType ? sfGoType : 0) | (f & SymFlagReflectMethod ? sfReflect : 0) |
	       (symFlag2(s) & SymFlagItab ? sfItab : 0) | (kindIsText(k) ? sfText : 0);
}

// 1. one pass over every unit's symbol records, in order: every slot starts as its own canonical symbol;
// definitions and references by name and content-addressed definitions leave a record for their join
static void resolveRecords(void *ctx UNUSED, int w, long ui) {
	unit *u = &units[ui];
	resolveWorker *rw = &rws[w];
	object *o = u->o;
	u32 npLo = o->ndef + o->nh64 + o->nh;
	for (u32 g = o->base + u->lo, e = o->base + u->hi; g < e; g++) canon[g] = g;
	const u8 *s = osym(o, MAX(u->lo, o->ndef));
	const u8 *ri = o->b + o->off[BlkRelocIdx];
	for (u32 j = MAX(u->lo, o->ndef); j < u->hi; j++, s += SymSize) {
		u32 gid = o->base + j;
		if (j < o->nalldef) SF(gid) = sfOf(s, ri + (size_t)j * 4);
		if (j < npLo) {
			// content-addressed: joined after the flood, and only if it was reached (resolveHashed)
		} else {
			u32 rk = 0;
			if (isDwarfKind(symType(s))) {
				// Debug information: this linker writes none (-w), and only debug information refers to it.
				// Four of five references by name are of this kind. A reference stays unresolved (0), so
				// that anything that did use it would say "undefined" rather than read a non-definition.
				if (j >= o->nalldef) {
					canon[gid] = 0;
					SF(gid) = sfRedir;
				}
				continue;
			}
			if (j < o->nalldef) {
				if (symABI(s) == SymABIstatic || symNameLen(s) == 0) continue; // file-local, or an unnamed auxiliary symbol
				rk = rankOf(o, j, s);
			} else if (symUsedInIface(s)) {
				rk = rankIface;
			}
			key128 k = nameKey128(o->b + ld32(s + 4), symNameLen(s), symABI(s));
			recAdd(rw, &rw->names[k.k1 >> 56], (rec){k.k1, k.k2, (u64)rk << 32 | (u32)~gid});
		}
	}
}

// better: the winner of two records of one name; the interface flag rides along
static inline u64 better(u64 a, u64 b) {
	u64 iface = (a | b) & valIface;
	a &= ~valIface;
	b &= ~valIface;
	if ((a >> 62) == 3 && (b >> 62) == 3 && a != b) {
		u32 g1 = ~(u32)a, g2 = ~(u32)b, j;
		str nm = symName(g1);
		object *o1 = objOf(g1, &j), *o2 = objOf(g2, &j);
		errorf("duplicated definition of symbol %.*s, from %.*s and %.*s", (int)nm.n, nm.p, (int)o1->pkg.n, o1->pkg.p, (int)o2->pkg.n, o2->pkg.p);
	}
	return (a > b ? a : b) | iface;
}

static inline void scratchFor(resolveWorker *rw, u64 n, u32 *size) {
	u32 sz = 64;
	while (sz < 2 * n) sz <<= 1;
	if (rw->scratchCap < sz) {
		rw->scratch = hugealloc(4 * (size_t)sz);
		rw->ent = hugealloc(sizeof(tent) * (size_t)sz / 2 + sizeof(tent));
		rw->scratchCap = sz;
	}
	memset(rw->scratch, 0, 4 * (size_t)sz);
	*size = sz;
}

// 2. the join by name, one partition at a time. A partition leaves its table for the lookups to come.
static void resolveJoin(void *ctx UNUSED, int w, long task) {
	resolveWorker *rw = &rws[w];
	{
		int p = (int)task;
		u64 n = 0;
		for (int t = 0; t < nthreads; t++)
			for (chunk *c = rws[t].names[p].head; c; c = c->next) n += c->n;
		if (n == 0) return;
		u32 size, nd = 0;
		scratchFor(rw, n, &size);
		u32 *idx = rw->scratch, mask = size - 1;
		tent *ent = rw->ent;
		for (int t = 0; t < nthreads; t++)
			for (chunk *c = rws[t].names[p].head; c; c = c->next)
				for (u32 i = 0; i < c->n; i++) {
					rec *r = &c->r[i];
					for (u32 h = (u32)(r->k1 >> 8) & mask;; h = (h + 1) & mask) {
						u32 e = idx[h];
						if (!e) {
							idx[h] = ++nd;
							ent[nd - 1] = (tent){r->k1, r->k2, r->val};
							break;
						}
						if (ent[e - 1].k1 == r->k1 && ent[e - 1].k2 == r->k2) {
							ent[e - 1].val = better(ent[e - 1].val, r->val);
							break;
						}
					}
				}
		u32 tsize = 16;
		while (tsize < 2 * nd) tsize <<= 1;
		tent *te = slabAlloc(rw, sizeof(tent) * (size_t)tsize);
		memset(te, 0, sizeof(tent) * (size_t)tsize);
		for (u32 i = 0; i < nd; i++) {
			for (u32 h = (u32)(ent[i].k1 >> 8) & (tsize - 1);; h = (h + 1) & (tsize - 1))
				if (!te[h].k1) {
					te[h] = ent[i];
					break;
				}
			u64 b1 = ent[i].k2 % bloomBits, b2 = (ent[i].k2 >> 32) % bloomBits;
			__atomic_fetch_or(&bloom[b1 >> 6], 1ull << (b1 & 63), __ATOMIC_RELAXED);
			__atomic_fetch_or(&bloom[b2 >> 6], 1ull << (b2 & 63), __ATOMIC_RELAXED);
		}
		nameTab[p].e = te;
		nameTab[p].mask = tsize - 1;
		rw->nrec[0] += (long)n;
		return;
	}
}

// ---- content-addressed symbols: joined after the flood, and only those it reached
//
// A content-addressed symbol (a string, a pc-value table, a stack map: half of all slots) can only be referred
// to from its own object, and two with one hash have the same bytes. So nothing before the flood needs to know
// which copy is canonical: the flood treats every copy as itself, and what it reached, about half of them, is
// joined here. The winner of a class is the largest copy (short hashes cover up to 8 bytes of content, and a
// 4-byte symbol may share its hash with an 8-byte one), then the lowest id, among the reached copies; it
// inherits the flags of all of them. Which copies were reached does not depend on thread timing (every
// reached copy that has relocations is flooded, not only the first seen), so neither does the winner.
#define hashedFlags (stLive | stPC | stFuncdata | stNameNeeded)
// A record of this join is 16 bytes, four to a cache line: 96 bits of the hash and the id. (For a short hash,
// k1 is the whole 8 bytes of content and k2 the symbol's size: the largest copy wins.) They are collected in
// chunks like the records by name.
typedef struct {
	u64 k1;
	u32 k2, gid;
} hrec;
#define hchunkRecs 126
typedef struct hchunk {
	struct hchunk *next;
	u64 n;
	hrec r[hchunkRecs];
} hchunk;
typedef struct {
	hchunk *head, *tail;
} hlist;
#define nShortParts 16
typedef struct {
	hlist longs[nHashParts], shorts[nShortParts];
	char pad[64];
} hashWorker;
static hashWorker *hws;
static inline void hrecAdd(resolveWorker *w, hlist *l, hrec r) {
	hchunk *c = l->tail;
	if (!c || c->n == hchunkRecs) {
		hchunk *d = slabAlloc(w, sizeof(hchunk));
		d->next = NULL;
		d->n = 0;
		if (c) c->next = d;
		else l->head = d;
		l->tail = c = d;
	}
	c->r[c->n++] = r;
}
static void hashedRecords(void *ctx UNUSED, int w, long ui) {
	unit *u = &units[ui];
	resolveWorker *rw = &rws[w];
	hashWorker *hw = &hws[w];
	object *o = u->o;
	u32 h64Lo = o->ndef, hLo = o->ndef + o->nh64, npLo = hLo + o->nh;
	const u8 *h64 = o->b + o->off[BlkHash64], *hh = o->b + o->off[BlkHash];
	for (u32 j = MAX(u->lo, h64Lo); j < MIN(u->hi, npLo); j++) {
		u32 gid = o->base + j;
		if ((FL(gid) & hashedFlags) == 0) continue;
		if (j < hLo) {
			u64 h = ld64(h64 + (size_t)(j - h64Lo) * 8);
			hrecAdd(rw, &hw->shorts[mix64(h) >> (64 - 4)], (hrec){h, symSiz(osym(o, j)), gid});
		} else {
			const u8 *h = hh + (size_t)(j - hLo) * HashSize;
			u64 k1 = ld64(h);
			hrecAdd(rw, &hw->longs[k1 >> (64 - 9)], (hrec){k1, ld32(h + 8), gid});
		}
	}
}
// The winner of a class: of long hashes, the lowest id; of short hashes, the largest copy, then the lowest id.
static void hashedJoin(void *ctx UNUSED, int w, long task) {
	resolveWorker *rw = &rws[w];
	bool isShort = task >= nHashParts;
	u64 n = 0;
#define HLIST(t) (isShort ? &hws[t].shorts[task - nHashParts] : &hws[t].longs[task])
	for (int t = 0; t < nthreads; t++)
		for (hchunk *c = HLIST(t)->head; c; c = c->next) n += c->n;
	if (n == 0) return;
	u32 size, nd = 0;
	scratchFor(rw, n, &size);
	u32 *idx = rw->scratch, mask = size - 1;
	hrec *ent = (hrec *)rw->ent; // (a group's key and its winner so far)
	for (int t = 0; t < nthreads; t++)
		for (hchunk *c = HLIST(t)->head; c; c = c->next)
			for (u32 i = 0; i < c->n; i++) {
				hrec *r = &c->r[i];
				u64 k = isShort ? mix64(r->k1) : r->k1; // (a long hash's top 9 bits picked the partition)
				for (u32 h = (u32)k & mask;; h = (h + 1) & mask) {
					u32 e = idx[h];
					if (!e) {
						idx[h] = ++nd;
						ent[nd - 1] = *r;
						break;
					}
					hrec *g = &ent[e - 1];
					if (g->k1 != r->k1) continue;
					if (isShort) {
						if (r->k2 > g->k2 || (r->k2 == g->k2 && r->gid < g->gid)) *g = *r;
						break;
					}
					if (g->k2 == r->k2) {
						if (r->gid < g->gid) g->gid = r->gid;
						break;
					}
				}
			}
	// every copy learns its class's winner (found again by its key: the tables are in this core's cache)
	for (int t = 0; t < nthreads; t++)
		for (hchunk *c = HLIST(t)->head; c; c = c->next)
			for (u32 i = 0; i < c->n; i++) {
				hrec *r = &c->r[i];
				u64 k = isShort ? mix64(r->k1) : r->k1;
				u32 win;
				for (u32 h = (u32)k & mask;; h = (h + 1) & mask) {
					hrec *g = &ent[idx[h] - 1];
					if (g->k1 == r->k1 && (isShort || g->k2 == r->k2)) {
						win = g->gid;
						break;
					}
				}
				if (win != r->gid) {
					canon[r->gid] = win;
					SF(r->gid) |= sfRedir;
					FL(win) |= FL(r->gid) & hashedFlags; // (a class is in one partition: no other thread writes these)
				}
			}
#undef HLIST
	rw->nrec[1] += (long)n;
	rw->nrec[2] += (long)nd;
}
static long nHashedReached, nHashedClasses;
static void resolveHashed(void) {
	hws = calloc((size_t)nthreads, sizeof(hashWorker));
	par(nunits, hashedRecords, NULL);
	par(nHashParts + nShortParts, hashedJoin, NULL);
	for (int t = 0; t < nthreads; t++) {
		nHashedReached += rws[t].nrec[1];
		nHashedClasses += rws[t].nrec[2];
	}
}

// 3. definitions by package index: themselves, unless something may refer to their name
static void resolvePkgDefs(void *ctx UNUSED, int w, long ui) {
	unit *u = &units[ui];
	resolveWorker *rw = &rws[w];
	object *o = u->o;
	u32 hi = MIN(u->hi, o->ndef);
	const u8 *s = osym(o, u->lo);
	const u8 *ri = o->b + o->off[BlkRelocIdx];
	for (u32 j = u->lo; j < hi; j++, s += SymSize) {
		u32 gid = o->base + j;
		SF(gid) = sfOf(s, ri + (size_t)j * 4);
		if (symABI(s) != SymABIstatic && symNameLen(s) != 0 && !isDwarfKind(symType(s))) {
			key128 k = nameKey128(o->b + ld32(s + 4), symNameLen(s), symABI(s));
			u64 b1 = k.k2 % bloomBits, b2 = (k.k2 >> 32) % bloomBits;
			tent *e;
			if ((bloom[b1 >> 6] >> (b1 & 63) & 1) && (bloom[b2 >> 6] >> (b2 & 63) & 1) && (e = nameFind(k)) != NULL) {
				u64 val = (u64)rankOf(o, j, s) << 32 | (u32)~gid, old = __atomic_load_n(&e->val, __ATOMIC_RELAXED);
				while (!__atomic_compare_exchange_n(&e->val, &old, better(old, val), true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
				// its record joins the others of its name, so that step 4 tells the slot if it lost
				recAdd(rw, &rw->names[k.k1 >> 56], (rec){k.k1, k.k2, val});
				rw->npkgjoin++;
				continue;
			}
		}
		if (symUsedInIface(s)) atomicOr8(&FL(gid), stUsedInIface);
	}
}

// 4. every record by name tells its slot the winner; the winners get their flags
static void resolveNames(void *ctx UNUSED, int w UNUSED, long p) {
	for (int t = 0; t < nthreads; t++)
		for (chunk *c = rws[t].names[p].head; c; c = c->next)
			for (u32 i = 0; i < c->n; i++) {
				rec *r = &c->r[i];
				u32 gid = ~(u32)r->val;
				if (gid == ~0u) continue; // a name the linker wants: not a slot
				u32 win = ~(u32)nameFind((key128){r->k1, r->k2})->val;
				if (win != gid) {
					canon[gid] = win;
					SF(gid) |= sfRedir;
				}
			}
	tent *te = nameTab[p].e;
	if (!te) return;
	for (u32 h = 0; h <= nameTab[p].mask; h++) {
		if (!te[h].k1) continue;
		u32 c = ~(u32)te[h].val;
		if (c == ~0u) continue;
		u32 f = (te[h].val >> 32 & rankDef ? 0 : stUndef) | (te[h].val & valIface ? stUsedInIface : 0);
		if (f) atomicOr8(&FL(c), (u8)f);
	}
}

// 4. "this type is converted to an interface" flags on references by package index
static void resolveRefFlags(void *ctx UNUSED, int w UNUSED, long i) {
	object *o = objs[i];
	u32 n = oNRefFlags(o);
	const u8 *rf = o->b + o->off[BlkRefFlags];
	for (u32 k = 0; k < n; k++, rf += RefFlagsSize) {
		if (rf[9] & SymFlagUsedInIface) {
			u32 g = oref(o, ld32(rf), ld32(rf + 4));
			if (g != 0) atomicOr8(&FL(canonOf(g)), stUsedInIface);
		}
	}
}

static void resolve(void) {
	idCap = nslots + 1 + 65536;
	nids = nslots + 1;
	canon = hugealloc((size_t)idCap * 4);
	state = hugealloc((size_t)idCap * 4);
	addr = hugealloc((size_t)idCap * 4);
	aux = hugealloc((size_t)idCap * 4);
	synthNames = calloc(65536, sizeof(str));
	bloom = hugealloc(bloomBits / 8);
	flsf = hugealloc(2 * (size_t)idCap);
	rws = calloc((size_t)nthreads, sizeof(resolveWorker));

	for (int i = 0; i < NBUILTIN; i++) wantName(cstr(builtinNames[i].name), (u16)builtinNames[i].abi);
	for (size_t i = 0; i < sizeof linkerNames / sizeof linkerNames[0]; i++) {
		wantName(cstr(linkerNames[i]), 0);
		wantName(cstr(linkerNames[i]), 1);
	}
	for (size_t i = 0; i < strdata.n; i++) wantName(cstr(strdata.a[i].name), 0);
	wantName(cstr(flagEntry), 0);
	wantName(cstr(flagEntry), 1);
	double t0 = nowSeconds();
	par(nunits, resolveRecords, NULL);
	double t1 = nowSeconds();
	par(nParts, resolveJoin, NULL);
	double t2 = nowSeconds();
	par(nunits, resolvePkgDefs, NULL);
	double t3 = nowSeconds();
	par(nParts, resolveNames, NULL);
	double t4 = nowSeconds();
	for (int i = 0; i < NBUILTIN; i++) {
		u32 g = lookupName(cstr(builtinNames[i].name), (u16)builtinNames[i].abi);
		if (isDef(g)) builtins[i] = g;
	}
	par(nobjs, resolveRefFlags, NULL);
	long nrec[3] = {0, 0, 0}, npkg = 0;
	for (int t = 0; t < nthreads; t++) {
		nrec[0] += rws[t].nrec[0];
		nrec[1] += rws[t].nrec[1];
		npkg += rws[t].npkgjoin;
	}
	lap("resolve", "%u slots; joined: %ld by name (and %ld definitions by package index); ms: records %.1f, joins %.1f, pkgdefs %.1f, names %.1f",
		nslots, nrec[0], npkg, (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3, (t4 - t3) * 1e3);
}
