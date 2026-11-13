// resolve.c: symbol resolution. Every slot of every object is resolved to the id of its canonical definition,
// by the stock loader's rules: package-indexed references are a table read; references by name join
// definitions by name and ABI; content-addressed symbols join by their hash.
//
// The joins are partitioned hash joins. One parallel pass over the units writes a record per symbol that takes
// part (key, id, rank) into one of 256 partitions chosen by the key's top bits; then each partition is grouped
// by key on its own thread and every record's slot learns its group's winner. The winner is the maximum of a
// total order, so the result does not depend on which thread saw what first:
//
//	a definition beats a reference; one that may not be duplicated beats one that may (DUPOK);
//	then one with content (or code) beats one without (BSS);
//	then the larger one wins (the stock loader's rule for DUPOK and BSS symbols and for hashed ones);
//	then the one in the earlier object, at the lower index.
//
// A name nothing defines resolves to its first reference: that slot stands for the undefined symbol.
// Definitions referenced by package index take part in the join by name only if some object refers to their
// name (or the linker itself looks it up): a bit filter of the referenced names decides.

#define nParts 256
#define filterBits (1u << 23)
#define rankNoDup (1u << 31)
#define rankContent (1u << 30)
#define rankDef (1u << 29)
#define rankIface (1u << 28) // (not part of the order: the UsedInIface flag rides along)

typedef struct {
	u64 key;
	const char *name;
	u32 nlen, gid, rank;
	u16 abi;
} nameRec;

typedef struct {
	u64 k1, k2;
	u32 gid, size;
} hashRec;

typedef struct {
	u64 key;
	u32 gid, size;
} hash64Rec;

// A plist is one worker's records of one partition of a join: chunks cut from the worker's slab, so that
// filling it never copies records. Every chunk but the last is full.
#define chunkRecs 512
typedef struct {
	void **chunks;
	int nch, cap;
	long n;
} plist;
typedef struct {
	u8 *p;
	long left; // records left in the slab
} slab;

static inline void *plistAdd(plist *l, slab *s, size_t esz) {
	if (l->n % chunkRecs == 0) {
		if (s->left < chunkRecs) {
			s->p = bigalloc(512 * chunkRecs * esz);
			s->left = 512 * chunkRecs;
		}
		if (l->nch == l->cap) {
			l->cap = l->cap ? 2 * l->cap : 8;
			l->chunks = realloc(l->chunks, (size_t)l->cap * sizeof(void *));
		}
		l->chunks[l->nch++] = s->p;
		s->p += chunkRecs * esz;
		s->left -= chunkRecs;
	}
	void *r = (u8 *)l->chunks[l->nch - 1] + (size_t)(l->n % chunkRecs) * esz;
	l->n++;
	return r;
}
static inline long plistChunkLen(const plist *l, int c) { return c < l->nch - 1 ? chunkRecs : l->n - (long)c * chunkRecs; }

typedef struct {
	plist names[nParts], hashes[nParts], h64s[nParts];
	slab nslab, hslab, h6slab;
	u32 *table;
	size_t tableCap;
	u32 *grp;
	u64 *best;
	u8 *ndup;
	void **flat;
	size_t grpCap;
	char pad[64];
} resolveWorker;

static resolveWorker *rws;
static u64 *nameFilter;

static inline void filterSet(u64 key) {
	u64 k = key % filterBits;
	__atomic_fetch_or(&nameFilter[k >> 6], 1ull << (k & 63), __ATOMIC_RELAXED);
}
static inline bool filterHas(u64 key) {
	u64 k = key % filterBits;
	return (nameFilter[k >> 6] >> (k & 63)) & 1;
}
static void wantName(str name, u16 abi) { filterSet(nameKey(name, abi)); }

static u32 lookupName(str name, u16 abi) {
	u64 key = nameKey(name, abi);
	for (int w = 0; w < nthreads; w++) {
		plist *l = &rws[w].names[key >> 56];
		for (int c = 0; c < l->nch; c++) {
			nameRec *recs = l->chunks[c];
			long n = plistChunkLen(l, c);
			for (long i = 0; i < n; i++) {
				nameRec *r = &recs[i];
				if (r->key == key && r->abi == abi && r->nlen == name.n && memcmp(r->name, name.p, name.n) == 0) return canon[r->gid];
			}
		}
	}
	return 0;
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

// 1. records for the definitions and references by name and for the content-addressed definitions
static void resolveRecords(void *ctx UNUSED, int w, long ui) {
	unit *u = &units[ui];
	resolveWorker *rw = &rws[w];
	object *o = u->o;
	u32 h64End = o->ndef + o->nh64, hEnd = o->ndef + o->nh64 + o->nh;
	for (u32 j = MAX(u->lo, o->ndef); j < u->hi; j++) {
		const u8 *s = osym(o, j);
		u32 gid = o->base + j;
		if (j < h64End) {
			u64 h = ld64(o->b + o->off[BlkHash64] + (size_t)(j - o->ndef) * Hash64Size);
			u64 k = mix64(h);
			hash64Rec *r = plistAdd(&rw->h64s[k >> 56], &rw->h6slab, sizeof(hash64Rec));
			*r = (hash64Rec){h, gid, symSiz(s)};
		} else if (j < hEnd) {
			const u8 *h = o->b + o->off[BlkHash] + (size_t)(j - h64End) * HashSize;
			u64 k1 = ld64(h), k2 = ld64(h + 8);
			hashRec *r = plistAdd(&rw->hashes[k1 >> 56], &rw->hslab, sizeof(hashRec));
			*r = (hashRec){k1, k2, gid, symSiz(s)};
		} else {
			u32 rk = 0;
			if (j < o->nalldef) {
				if (symABI(s) == SymABIstatic || symNameLen(s) == 0) {
					canon[gid] = gid; // file-local, or an unnamed auxiliary symbol
					continue;
				}
				rk = rankOf(o, j, s);
			} else if (symUsedInIface(s)) {
				rk = rankIface;
			}
			str name = symNameOf(o, s);
			u64 key = nameKey(name, symABI(s));
			nameRec *r = plistAdd(&rw->names[key >> 56], &rw->nslab, sizeof(nameRec));
			*r = (nameRec){key, name.p, name.n, gid, rk, symABI(s)};
			filterSet(key);
		}
	}
}

// 2. definitions by package index: themselves, unless something may refer to their name
static void resolvePkgDefs(void *ctx UNUSED, int w, long ui) {
	unit *u = &units[ui];
	resolveWorker *rw = &rws[w];
	object *o = u->o;
	for (u32 j = u->lo; j < MIN(u->hi, o->ndef); j++) {
		const u8 *s = osym(o, j);
		u32 gid = o->base + j;
		if (symABI(s) != SymABIstatic && symNameLen(s) != 0) {
			str name = symNameOf(o, s);
			u64 key = nameKey(name, symABI(s));
			if (filterHas(key)) {
				nameRec *r = plistAdd(&rw->names[key >> 56], &rw->nslab, sizeof(nameRec));
				*r = (nameRec){key, name.p, name.n, gid, rankOf(o, j, s), symABI(s)};
				continue;
			}
		}
		canon[gid] = gid;
		if (symUsedInIface(s)) atomicOr32(&state[gid], stUsedInIface);
	}
}

static long nrec[3];

// 3. the joins, one partition at a time
static void resolveJoin(void *ctx UNUSED, int w, long task) {
	resolveWorker *rw = &rws[w];
	int p = (int)(task % nParts), kind = (int)(task / nParts);
	long n = 0;
	for (int i = 0; i < nthreads; i++) {
		plist *l = kind == 0 ? &rws[i].names[p] : kind == 1 ? &rws[i].hashes[p] : &rws[i].h64s[p];
		n += l->n;
	}
	if (n == 0) return;
	__atomic_fetch_add(&nrec[kind], n, __ATOMIC_RELAXED);
	size_t size = 64;
	while (size < 2 * (size_t)n) size <<= 1;
	if (rw->tableCap < size) {
		free(rw->table);
		rw->table = malloc(size * sizeof(u32));
		rw->tableCap = size;
	}
	if (rw->grpCap < (size_t)n) {
		size_t c = (size_t)n + (size_t)n / 2;
		free(rw->grp);
		free(rw->best);
		free(rw->ndup);
		free(rw->flat);
		rw->grp = malloc(c * sizeof(u32));
		rw->best = malloc(c * sizeof(u64));
		rw->ndup = malloc(c);
		rw->flat = malloc(c * sizeof(void *));
		rw->grpCap = c;
	}
	u32 *table = rw->table, *grp = rw->grp;
	u64 *best = rw->best;
	u8 *ndup = rw->ndup;
	void **flat = rw->flat;
	memset(table, 0, size * sizeof(u32));
	u64 mask = size - 1;
	// group: grp[i] is the index (over the partition's records, in worker order) of the first record with
	// record i's key; best[that] is the group's maximum of rank<<32 | ^id
	u32 i = 0;
	switch (kind) {
	case 0:
		for (int wi = 0; wi < nthreads; wi++) {
			plist *l = &rws[wi].names[p];
			for (int c = 0; c < l->nch; c++) {
				nameRec *recs = l->chunks[c];
				long cn = plistChunkLen(l, c);
				for (long k = 0; k < cn; k++) {
					nameRec *r = &recs[k];
					flat[i] = r;
					u64 val = (u64)(r->rank & ~rankIface) << 32 | (u32)~r->gid;
					u64 h = r->key & mask;
					for (;;) {
						u32 t = table[h];
						if (t == 0) {
							table[h] = i + 1;
							grp[i] = i;
							best[i] = val;
							ndup[i] = (u8)(r->rank >> 28 & 1);
							break;
						}
						nameRec *f = flat[t - 1];
						if (f->key == r->key && f->abi == r->abi && f->nlen == r->nlen && memcmp(f->name, r->name, r->nlen) == 0) {
							grp[i] = t - 1;
							if ((r->rank & (rankNoDup | rankContent)) == (rankNoDup | rankContent) && best[t - 1] >> 62 == 3) {
								u32 j1, j2;
								object *o1 = objOf(r->gid, &j1), *o2 = objOf(~(u32)best[t - 1], &j2);
								errorf("duplicated definition of symbol %.*s, from %.*s and %.*s", (int)r->nlen, r->name, (int)o2->pkg.n,
								       o2->pkg.p, (int)o1->pkg.n, o1->pkg.p);
							}
							best[t - 1] = MAX(best[t - 1], val);
							ndup[t - 1] |= (u8)(r->rank >> 28 & 1);
							break;
						}
						h = (h + 1) & mask;
					}
					i++;
				}
			}
		}
		for (u32 k = 0; k < (u32)n; k++) {
			nameRec *r = flat[k];
			u32 g = grp[k];
			u32 c = ~(u32)best[g];
			canon[r->gid] = c;
			if (k == g) {
				if (((best[g] >> 32) & rankDef) == 0) atomicOr32(&state[c], stUndef);
				if (ndup[g] != 0) atomicOr32(&state[c], stUsedInIface);
			}
		}
		break;
	case 1:
		for (int wi = 0; wi < nthreads; wi++) {
			plist *l = &rws[wi].hashes[p];
			for (int c = 0; c < l->nch; c++) {
				hashRec *recs = l->chunks[c];
				long cn = plistChunkLen(l, c);
				for (long k = 0; k < cn; k++) {
					hashRec *r = &recs[k];
					flat[i] = r;
					u64 val = (u64)r->size << 32 | (u32)~r->gid;
					u64 h = r->k1 & mask;
					for (;;) {
						u32 t = table[h];
						if (t == 0) {
							table[h] = i + 1;
							grp[i] = i;
							best[i] = val;
							break;
						}
						hashRec *f = flat[t - 1];
						if (f->k1 == r->k1 && f->k2 == r->k2) {
							grp[i] = t - 1;
							best[t - 1] = MAX(best[t - 1], val);
							break;
						}
						h = (h + 1) & mask;
					}
					i++;
				}
			}
		}
		for (u32 k = 0; k < (u32)n; k++) canon[((hashRec *)flat[k])->gid] = ~(u32)best[grp[k]];
		break;
	case 2:
		for (int wi = 0; wi < nthreads; wi++) {
			plist *l = &rws[wi].h64s[p];
			for (int c = 0; c < l->nch; c++) {
				hash64Rec *recs = l->chunks[c];
				long cn = plistChunkLen(l, c);
				for (long k = 0; k < cn; k++) {
					hash64Rec *r = &recs[k];
					flat[i] = r;
					u64 val = (u64)r->size << 32 | (u32)~r->gid;
					u64 h = mix64(r->key) & mask;
					for (;;) {
						u32 t = table[h];
						if (t == 0) {
							table[h] = i + 1;
							grp[i] = i;
							best[i] = val;
							break;
						}
						if (((hash64Rec *)flat[t - 1])->key == r->key) {
							grp[i] = t - 1;
							best[t - 1] = MAX(best[t - 1], val);
							break;
						}
						h = (h + 1) & mask;
					}
					i++;
				}
			}
		}
		for (u32 k = 0; k < (u32)n; k++) canon[((hash64Rec *)flat[k])->gid] = ~(u32)best[grp[k]];
		break;
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
			if (g != 0) atomicOr32(&state[canon[g]], stUsedInIface);
		}
	}
}

static void resolve(void) {
	idCap = nslots + 1 + 65536;
	nids = nslots + 1;
	canon = bigalloc((size_t)idCap * 4);
	state = bigalloc((size_t)idCap * 4);
	addr = bigalloc((size_t)idCap * 4);
	aux = bigalloc((size_t)idCap * 4);
	synthNames = calloc(65536, sizeof(str));
	nameFilter = bigalloc(filterBits / 8);
	rws = calloc((size_t)nthreads, sizeof(resolveWorker));

	par(nunits, resolveRecords, NULL);
	for (int i = 0; i < NBUILTIN; i++) wantName(cstr(builtinNames[i].name), (u16)builtinNames[i].abi);
	for (size_t i = 0; i < sizeof linkerNames / sizeof linkerNames[0]; i++) {
		wantName(cstr(linkerNames[i]), 0);
		wantName(cstr(linkerNames[i]), 1);
	}
	for (size_t i = 0; i < strdata.n; i++) wantName(cstr(strdata.a[i].name), 0);
	wantName(cstr(flagEntry), 0);
	wantName(cstr(flagEntry), 1);
	par(nunits, resolvePkgDefs, NULL);
	par(3 * nParts, resolveJoin, NULL);
	for (int i = 0; i < NBUILTIN; i++) {
		u32 g = lookupName(cstr(builtinNames[i].name), (u16)builtinNames[i].abi);
		if (isDef(g)) builtins[i] = g;
	}
	par(nobjs, resolveRefFlags, NULL);
	lap("resolve", "%u slots; joined: %ld by name, %ld + %ld by content hash", nslots, nrec[0], nrec[1], nrec[2]);
}
