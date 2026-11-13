// dead.c: the init order, and reachability with the stock linker's method pruning (cmd/link/internal/ld
// deadcode.go), as a flood that all cores advance one frontier at a time. Marking is a fetch-or on the symbol's
// state word; whoever sets the bit queues the symbol for the next level.

static vecu32 mainInittasks, runtimeInittasks; // the inittask records the runtime runs at start, in order

// ---- a u32 -> int map (serial)
typedef struct {
	u32 *keys; // 0: empty (id 0 is never a key)
	int *vals;
	u64 mask;
	size_t n;
} u32map;
static void umInit(u32map *m, size_t hint) {
	size_t sz = 64;
	while (sz < 2 * hint) sz <<= 1;
	m->keys = calloc(sz, sizeof(u32));
	m->vals = calloc(sz, sizeof(int));
	m->mask = sz - 1;
	m->n = 0;
}
static int *umFind(u32map *m, u32 k) {
	u64 h = mix64(k) & m->mask;
	for (;;) {
		if (m->keys[h] == 0) return NULL;
		if (m->keys[h] == k) return &m->vals[h];
		h = (h + 1) & m->mask;
	}
}
static void umPut(u32map *m, u32 k, int v) {
	if (2 * (m->n + 1) > m->mask + 1) {
		u32map o = *m;
		umInit(m, o.mask + 1);
		for (size_t i = 0; i <= o.mask; i++)
			if (o.keys[i]) umPut(m, o.keys[i], o.vals[i]);
		free(o.keys);
		free(o.vals);
	}
	u64 h = mix64(k) & m->mask;
	for (;;) {
		if (m->keys[h] == 0) {
			m->keys[h] = k;
			m->vals[h] = v;
			m->n++;
			return;
		}
		if (m->keys[h] == k) {
			m->vals[h] = v;
			return;
		}
		h = (h + 1) & m->mask;
	}
}

// ---- a binary min-heap of (name, id), container/heap's algorithm
typedef struct {
	str name;
	u32 gid;
} nameEnt;
typedef VEC(nameEnt) nameHeap;
static bool heapLess(nameHeap *h, size_t i, size_t j) { return strcmpGo(h->a[i].name, h->a[j].name) < 0; }
static void heapSwap(nameHeap *h, size_t i, size_t j) {
	nameEnt t = h->a[i];
	h->a[i] = h->a[j];
	h->a[j] = t;
}
static void heapUp(nameHeap *h, size_t j) {
	while (j > 0) {
		size_t i = (j - 1) / 2;
		if (i == j || !heapLess(h, j, i)) break;
		heapSwap(h, i, j);
		j = i;
	}
}
static bool heapDown(nameHeap *h, size_t i0, size_t n) {
	size_t i = i0;
	for (;;) {
		size_t j1 = 2 * i + 1;
		if (j1 >= n) break;
		size_t j = j1;
		if (j1 + 1 < n && heapLess(h, j1 + 1, j1)) j = j1 + 1;
		if (!heapLess(h, j, i)) break;
		heapSwap(h, i, j);
		i = j;
	}
	return i > i0;
}
static void heapPush(nameHeap *h, u32 g) {
	nameEnt e = {symName(g), g};
	VPUSH(*h, e);
	heapUp(h, h->n - 1);
}
static u32 heapPop(nameHeap *h) {
	size_t n = h->n - 1;
	heapSwap(h, 0, n);
	heapDown(h, 0, n);
	return h->a[--h->n].gid;
}

typedef struct {
	u32 from, to;
} edge;
static int edgeCmp(const void *a, const void *b) {
	const edge *x = a, *y = b;
	if (x->to != y->to) return x->to < y->to ? -1 : 1;
	return 0;
}
// a stable merge sort of the edges by target
static void edgeSort(edge *e, size_t n) {
	if (n < 2) return;
	edge *tmp = malloc(n * sizeof(edge));
	for (size_t w = 1; w < n; w *= 2) {
		for (size_t lo = 0; lo < n; lo += 2 * w) {
			size_t mid = MIN(lo + w, n), hi = MIN(lo + 2 * w, n), a = lo, b = mid, k = lo;
			while (a < mid && b < hi) tmp[k++] = edgeCmp(&e[b], &e[a]) < 0 ? e[b++] : e[a++];
			while (a < mid) tmp[k++] = e[a++];
			while (b < hi) tmp[k++] = e[b++];
		}
		memcpy(e, tmp, n * sizeof(edge));
	}
	free(tmp);
}

// inittaskOrder: every inittask record reachable from root over R_INITORDER edges (package imports), in an order
// that respects the edges and is otherwise lexicographic by name; records with nothing to run are left out.
// This is ld.(*Link).inittaskSym.
static vecu32 inittaskOrder(const char *root) {
	vecu32 sched = {0};
	u32 r0 = lookupC(root, 0);
	if (!isDef(r0)) return sched;
	VEC(edge) edges = {0};
	u32map m;
	umInit(&m, 64);
	umPut(&m, r0, 0);
	vecu32 q = {0};
	VPUSH(q, r0);
	nameHeap h = {0};
	while (q.n > 0) {
		u32 x = q.a[--q.n];
		int ndeps = 0;
		if (isDef(x)) {
			u32 j, nr;
			object *o = objOf(x, &j);
			relocp rs = oRelocs(o, j, &nr);
			for (u32 i = 0; i < nr; i++) {
				relocp rl = RELOC(rs, i);
				if (rType(rl) != R_INITORDER) continue;
				ndeps++;
				u32 s = canonOf(RREF(o, rl));
				edge e = {x, s};
				VPUSH(edges, e);
				if (!umFind(&m, s)) {
					VPUSH(q, s);
					umPut(&m, s, 0);
				}
			}
		}
		umPut(&m, x, ndeps);
		if (ndeps == 0) heapPush(&h, x);
	}
	edgeSort(edges.a, edges.n);
	while (h.n > 0) {
		u32 s = heapPop(&h);
		if (isDef(s)) {
			u32 j;
			object *o = objOf(s, &j);
			if (symSiz(osym(o, j)) > 8) VPUSH(sched, s);
		}
		// sort.Search: the first edge with to >= s
		size_t lo = 0, hi = edges.n;
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;
			if (edges.a[mid].to >= s) hi = mid;
			else lo = mid + 1;
		}
		for (size_t a = lo; a < edges.n && edges.a[a].to == s; a++) {
			int *d = umFind(&m, edges.a[a].from);
			if (--*d == 0) heapPush(&h, edges.a[a].from);
		}
	}
	for (size_t i = 0; i <= m.mask; i++)
		if (m.keys[i] && m.vals[i] != 0) {
			str n = symName(m.keys[i]);
			fatalf("inittask for %.*s is not schedulable", (int)n.n, n.p);
		}
	free(edges.a);
	free(q.a);
	free(h.a);
	free(m.keys);
	free(m.vals);
	return sched;
}

static void inittasks(void) {
	mainInittasks = inittaskOrder("main..inittask");
	if (lookupC("runtime.runtime_inittasks", 0) != 0) runtimeInittasks = inittaskOrder("runtime..inittask");
	lap("inittasks", "%zu + %zu packages with initialization to run", mainInittasks.n, runtimeInittasks.n);
}

typedef struct {
	str name;
	u32 typ; // the method's function type
} methodsig;

// A methodref is a method of a live type that is converted to an interface: its three R_METHODOFF relocations
// (type, interface-call entry, direct entry) are followed only once something can call it.
typedef struct {
	methodsig sig;
	u32 src; // the receiver type
	u32 r;   // index of the first relocation
} methodref;

typedef struct {
	vecu32 next;
	VEC(methodref) methods;
	VEC(methodsig) ifaceM;
	VEC(str) generic;
	bool reflect;
	vecu32 tmp;
	struct fbatch *batch; // two: the batch being flooded and the next one, whose first stages run early
	u64 nflooded, nrelocs, nauxs; // what the flood read (for -stats and the floor)
	char pad[64];
} floodWorker;

static floodWorker *fws;

static inline void fmark(floodWorker *w, u32 c) {
	// A newly live definition is flooded, unless there is nothing to follow from it: no relocations, not a
	// function (whose aux symbols are followed), not marked as calling reflect's Method. Most content-addressed
	// symbols (strings, stack maps) are such leaves, and not queueing them saves a cache miss each.
	// (Most edges lead to a symbol that is live already: look before the locked operation, which, unlike a
	// plain load, waits for every miss before it.)
	if (c == 0 || (atomicLoad8(&FL(c)) & stLive)) return;
	if ((atomicOr8(&FL(c), stLive) & (stLive | stUndef)) == 0 && c <= nslots && (SF(c) & (sfHasRel | sfText | sfReflect)) != 0)
		VPUSH(w->next, c);
}

// setIface records that type c is converted to an interface. If it was already flooded as a plain type it is
// flooded again, this time following its methods' types and its element types as such.
static inline void setIface(floodWorker *w, u32 c) {
	u8 old = atomicOr8(&FL(c), stUsedInIface);
	if ((old & stUsedInIface) == 0 && (old & stLive) != 0 && (SF(c) & sfHasRel) != 0) VPUSH(w->next, c);
}

static inline relocp relocAt(relocp rs, u32 n, i32 off) {
	for (u32 i = 0; i < n; i++)
		if (rOff(RELOC(rs, i)) == off) return RELOC(rs, i);
	return NULL;
}

// nameData decodes a type:.namedata symbol: a flag byte, a varint length, the name.
static str nameData(bytes d) {
	if (d.n < 2) return (str){"", 0};
	u32 k;
	u64 n = uvarint(d.p + 1, d.n - 1, &k);
	if (k == 0 || 1 + k + n > d.n) return (str){"", 0}; // (Go would panic)
	return (str){(const char *)d.p + 1 + k, (u32)n};
}

static str nameAt(const object *o, relocp rs, u32 n, i32 off) {
	relocp rl = relocAt(rs, n, off);
	if (!rl) return (str){"", 0};
	return nameData(dataOf(canonOf(RREF(o, rl))));
}

static inline u8 typeKind(const u8 *p) { return p[23]; }

// the FuncInfo of a function (goobj.FuncInfo): lengths and inline tree nodes
static inline u32 fiNumInl(bytes fi, u32 *inlOff) {
	u32 nfile = ld32(fi.p + FUNCINFO_NUMFILE_OFF);
	u32 off = FUNCINFO_NUMFILE_OFF + 4 + 4 * nfile;
	*inlOff = off + 4;
	return ld32(fi.p + off);
}
// node k: Parent, File, Line, Func {PkgIdx, SymIdx}, ParentPC
static inline const u8 *fiInlNode(bytes fi, u32 inlOff, u32 k) { return fi.p + inlOff + k * INLTREE_NODE_SIZE; }

// process floods symbol c. tg[i] is the canonical target of its relocation i and atg[i] that of its aux record i
// (functions only): floodTask has worked them out, so that their flags are in the cache by now.
static void process(floodWorker *w, u32 c, bool uii, const u32 *tg, const u32 *atg) {
	u32 j;
	object *o = objOf(c, &j);
	const u8 *s = osym(o, j);
	u8 sfc = SF(c);
	if (sfc & sfReflect) w->reflect = true;
	// (uii was read when the batch's references were decoded. If the type is converted to an interface after
	// that, whoever sets the flag queues the type again: setIface.)
	if (uii && (atomicOr8(&FL(c), stIfaceDone) & stIfaceDone)) return;
	u32 nr;
	relocp rs = oRelocs(o, j, &nr);
	w->tmp.n = 0;
	for (u32 i = 0; i < nr; i++) {
		relocp rl = RELOC(rs, i);
		u16 t = rType(rl);
		if (t & R_WEAK) continue;
		switch (t) {
		case R_METHODOFF:
			if (i + 2 >= nr) {
				str n = symNameOf(o, s);
				fatalf("%.*s: expect three consecutive R_METHODOFF relocs", (int)n.n, n.p);
			}
			if (uii) {
				VPUSH(w->tmp, i);
				// the method's type is itself reachable through reflection
				u32 rsym = tg[i];
				if (rsym != 0) setIface(w, rsym);
			}
			i += 2;
			continue;
		case R_USETYPE:
		case R_INITORDER: continue;
		case R_USEIFACE: {
			u32 rsym = tg[i];
			if (!isDef(rsym)) continue;
			if (SF(rsym) & sfItab) { // "the type of that itab"
				u32 j2;
				object *o2 = objOf(rsym, &j2);
				u32 nr2;
				relocp rs2 = oRelocs(o2, j2, &nr2);
				relocp rl2 = relocAt(rs2, nr2, 8);
				if (!rl2) continue;
				rsym = canonOf(RREF(o2, rl2));
				if (!isDef(rsym)) continue;
			}
			setIface(w, rsym);
			continue;
		}
		case R_USEIFACEMETHOD: {
			u32 rsym = tg[i];
			if (isDef(rsym)) {
				u32 j2, nr2;
				object *o2 = objOf(rsym, &j2);
				relocp rr = oRelocs(o2, j2, &nr2);
				i32 off = (i32)rAdd(rl);
				methodsig m = {nameAt(o2, rr, nr2, off), 0};
				relocp rl2 = relocAt(rr, nr2, off + 4);
				if (rl2) m.typ = canonOf(RREF(o2, rl2));
				VPUSH(w->ifaceM, m);
			}
			continue;
		}
		case R_USENAMEDMETHOD: {
			bytes d = dataOf(tg[i]);
			str n = {(const char *)d.p, d.n};
			VPUSH(w->generic, n);
			continue;
		}
		}
		u32 rsym = tg[i];
		if (rsym == 0) continue;
		if (uii && (SF(rsym) & sfGoType) && isDef(rsym) && (atomicLoad8(&FL(rsym)) & stUsedInIface) == 0) {
			// a type reachable by reflection from one converted to an interface ([]T from T, ...)
			u8 old = atomicOr8(&FL(rsym), stUsedInIface | stLive);
			if ((old & stUsedInIface) == 0 || (old & stLive) == 0) VPUSH(w->next, rsym);
			continue;
		}
		fmark(w, rsym);
	}
	if (sfc & sfText) {
		u32 na;
		auxp as = oAuxs(o, j, &na);
		u32 ninl = 0;
		for (u32 i = 0; i < na; i++) {
			if (aType(AUX(as, i)) == AuxFuncInfo) {
				bytes fi = dataOf(atg[i]);
				if (fi.n == 0) break;
				u32 inlOff;
				ninl = fiNumInl(fi, &inlOff);
				for (u32 k = 0; k < ninl; k++) {
					const u8 *node = fiInlNode(fi, inlOff, k);
					u32 callee = oref(o, ld32(node + 12), ld32(node + 16));
					if (callee != 0) atomicOr8(&FL(canonOf(callee)), stNameNeeded);
				}
				break;
			}
		}
		for (u32 i = 0; i < na; i++) {
			auxp a = AUX(as, i);
			switch (aType(a)) {
			case AuxFuncdata: {
				u32 cc = atg[i];
				if (cc != 0) {
					atomicOr8(&FL(cc), stFuncdata);
					fmark(w, cc);
				}
				break;
			}
			case AuxPcinline:
				if (ninl == 0) break;
				/* fallthrough */
			case AuxPcsp:
			case AuxPcfile:
			case AuxPcline:
			case AuxPcdata: {
				if (atg[i] != 0 && !(atomicLoad8(&FL(atg[i])) & stPC)) atomicOr8(&FL(atg[i]), stPC);
				break;
			}
			}
		}
	}
	if (w->tmp.n != 0) {
		// which methods does this type have? (name and function type of each, from its uncommon type)
		bytes p = oData(o, j);
		if (p.n <= ABI_TFlagOff8 || (p.p[ABI_TFlagOff8] & ABI_TFlagUncommon) == 0) {
			str n = symNameOf(o, s);
			fatalf("%.*s: methods on a type with no uncommon type", (int)n.n, n.p);
		}
		u8 kind = typeKind(p.p);
		int off = kind < ABI_NKIND ? abiRTypeSize8[kind] : 48;
		u32 mcount = ld16(p.p + off + 4);
		u32 moff = ld32(p.p + off + 8);
		if (mcount != w->tmp.n) {
			str n = symNameOf(o, s);
			fatalf("%.*s: %zu method relocations for %u methods", (int)n.n, n.p, w->tmp.n, mcount);
		}
		for (size_t i = 0; i < w->tmp.n; i++) {
			u32 ri = w->tmp.a[i];
			i32 at = (i32)(off + moff + 16 * i);
			str name;
			if (ri > 0 && rOff(RELOC(rs, ri - 1)) == at) name = nameData(dataOf(tg[ri - 1]));
			else name = nameAt(o, rs, nr, at);
			methodref m = {{name, tg[ri]}, c, ri};
			VPUSH(w->methods, m);
		}
	}
}

// isExported: the first rune is an upper-case letter (unicode.IsUpper)
static bool isExported(str name) {
	if (name.n == 0) return false;
	u8 c = (u8)name.p[0];
	if (c < 0x80) return c >= 'A' && c <= 'Z';
	u32 r;
	int n;
	if ((c & 0xE0) == 0xC0) { r = c & 0x1F; n = 1; }
	else if ((c & 0xF0) == 0xE0) { r = c & 0x0F; n = 2; }
	else if ((c & 0xF8) == 0xF0) { r = c & 0x07; n = 3; }
	else return false;
	if (name.n < (u32)n + 1) return false;
	for (int i = 1; i <= n; i++) r = r << 6 | ((u8)name.p[i] & 0x3F);
	return iswupper((wint_t)r) != 0;
}

// ---- sets of method signatures and names, built serially between rounds, read in parallel
typedef struct {
	methodsig *keys;
	bool *used;
	u64 mask;
	size_t n;
} sigset;
static inline u64 sigHash(methodsig m) { return hashBytes(m.name.p, m.name.n) ^ mix64(m.typ + 1); }
static void ssInit(sigset *s, size_t hint) {
	size_t sz = 64;
	while (sz < 2 * hint) sz <<= 1;
	s->keys = calloc(sz, sizeof(methodsig));
	s->used = calloc(sz, 1);
	s->mask = sz - 1;
	s->n = 0;
}
static bool ssHas(const sigset *s, methodsig m, bool byName) {
	u64 h = (byName ? hashBytes(m.name.p, m.name.n) : sigHash(m)) & s->mask;
	for (;;) {
		if (!s->used[h]) return false;
		if (streq(s->keys[h].name, m.name) && (byName || s->keys[h].typ == m.typ)) return true;
		h = (h + 1) & s->mask;
	}
}
static void ssAdd(sigset *s, methodsig m, bool byName) {
	if (2 * (s->n + 1) > s->mask + 1) {
		sigset o = *s;
		ssInit(s, o.mask + 1);
		for (size_t i = 0; i <= o.mask; i++)
			if (o.used[i]) ssAdd(s, o.keys[i], byName);
		free(o.keys);
		free(o.used);
	}
	if (ssHas(s, m, byName)) return;
	u64 h = (byName ? hashBytes(m.name.p, m.name.n) : sigHash(m)) & s->mask;
	while (s->used[h]) h = (h + 1) & s->mask;
	s->used[h] = true;
	s->keys[h] = m;
	s->n++;
}

#define floodGrain 1024
#define methodGrain 1024
static vecu32 frontier;
static VEC(methodref) markable;
static bool *keepM;
static sigset ifaceMethod, genericIfaceMethod;
static bool reflectSeen;

// floodTask floods a stretch of the frontier. Flooding a symbol is a chain of cache misses, each address
// known only when the one before has arrived: its index entries, its relocations and aux records, the flags
// of what they name (and the canonical id, for a duplicate), a function's FuncInfo. Done symbol by symbol the
// chain runs at the speed of one miss at a time. So the stretch is cut into batches of 64 symbols that go
// through stages; each stage reads what an earlier one asked for and asks for what a later one will read, so
// that the misses of a batch overlap; and the first two stages of the next batch run inside this one's, so
// that its relocations have a batch's time to arrive. The references are decoded once, into target arrays
// that the last stage (process) consumes.
#define floodBatch 64
typedef struct fbatch {
	const u32 *f; // the symbols
	size_t n;
	struct {
		object *o;
		u32 j, nr, na, t0, a0;
		relocp rs;
		auxp as;
		bool uii; // it is a type converted to an interface (as seen when its references were decoded)
	} it[floodBatch];
	u32 nt, nat;
	vecu32 tg, atg; // the canonical targets of the batch's relocations and aux records
} fbatch;
static u64 tscStage[256][8]; // (per worker, capped: see tscW)
#define tscW(w) ((w) & 255)

static inline void bIndex(fbatch *b) {
	for (size_t k = 0; k < b->n; k++) {
		b->it[k].o = objOf(b->f[k], &b->it[k].j);
		__builtin_prefetch(b->it[k].o->b + b->it[k].o->off[BlkRelocIdx] + b->it[k].j * 4);
		if (SF(b->f[k]) & sfText) __builtin_prefetch(b->it[k].o->b + b->it[k].o->off[BlkAuxIdx] + b->it[k].j * 4);
	}
}
static inline void bRelocs(fbatch *b) {
	u32 nt = 0, nat = 0;
	for (size_t k = 0; k < b->n; k++) {
		b->it[k].rs = oRelocs(b->it[k].o, b->it[k].j, &b->it[k].nr);
		b->it[k].t0 = nt;
		nt += b->it[k].nr;
		for (u32 x = 0; x < b->it[k].nr * RelocSize && x < 64 * 64; x += 64) __builtin_prefetch(b->it[k].rs + x);
		b->it[k].na = 0;
		b->it[k].a0 = nat;
		if (SF(b->f[k]) & sfText) {
			b->it[k].as = oAuxs(b->it[k].o, b->it[k].j, &b->it[k].na);
			nat += b->it[k].na;
			for (u32 x = 0; x < b->it[k].na * AuxSize && x < 8 * 64; x += 64) __builtin_prefetch(b->it[k].as + x);
		}
	}
	b->nt = nt;
	b->nat = nat;
	VRESERVE(b->tg, nt + 1);
	VRESERVE(b->atg, nat + 1);
}
// The references the flood will follow, decoded, and the flags of what they name asked for. What it will not
// follow is left 0 and never decoded (a reference into a package that is not in the importcfg must not fail
// the link unless something uses it): weak relocations, R_USETYPE and R_INITORDER, the methods of a type that
// is not converted to an interface (they are followed when something can call them: methodTask), and the aux
// records that are debug information.
static inline void bDecode(fbatch *b) {
	for (size_t k = 0; k < b->n; k++) {
		object *o = b->it[k].o;
		u32 *t = b->tg.a + b->it[k].t0, c = b->f[k];
		bool uii = (SF(c) & sfGoType) && (atomicLoad8(&FL(c)) & stUsedInIface);
		b->it[k].uii = uii;
		for (u32 r = 0; r < b->it[k].nr; r++) {
			relocp rl = RELOC(b->it[k].rs, r);
			u16 ty = rType(rl);
			u32 g = 0;
			if (!((ty & R_WEAK) || ty == R_USETYPE || ty == R_INITORDER || (ty == R_METHODOFF && !uii))) {
				g = RREF(o, rl);
				__builtin_prefetch(&FL(g));
			}
			t[r] = g;
			// A weak relocation or a method entry is written whether or not the flood follows it: with its
			// target's address if the target is live, with a placeholder if not. When the target is a
			// content-addressed copy, another copy of it may be the live one; the copy named here is flagged
			// so that the join after the flood (resolveHashed) includes it and it learns its class's winner.
			if (((ty & R_WEAK) || ty == R_METHODOFF) && rSiz(rl) != 0 && (rPkg(rl) == PkgIdxHashed || rPkg(rl) == PkgIdxHashed64)) {
				u32 h = RREF(o, rl);
				if (!(atomicLoad8(&FL(h)) & flRef)) atomicOr8(&FL(h), flRef);
			}
		}
		t = b->atg.a + b->it[k].a0;
		for (u32 a = 0; a < b->it[k].na; a++) {
			auxp ax = AUX(b->it[k].as, a);
			u8 ty = aType(ax);
			u32 g = 0;
			if (ty == AuxFuncInfo || ty == AuxFuncdata || (ty >= AuxPcsp && ty <= AuxPcdata)) {
				g = AREF(o, ax);
				__builtin_prefetch(&FL(g));
			}
			t[a] = g;
		}
	}
}
// the canonical ids of the duplicates among them, then those ids' flags
static inline void bDups(fbatch *b, int w) {
	u32 *tg = b->tg.a, *atg = b->atg.a, nt = b->nt, nat = b->nat;
	u64 t0 = __rdtsc();
	for (u32 x = 0; x < nt; x++)
		if (SF(tg[x]) & sfRedir) __builtin_prefetch(&canon[tg[x]]);
	for (u32 x = 0; x < nat; x++)
		if (SF(atg[x]) & sfRedir) __builtin_prefetch(&canon[atg[x]]);
	u64 t1 = __rdtsc();
	tscStage[tscW(w)][3] += t1 - t0;
	for (u32 x = 0; x < nt; x++)
		if (SF(tg[x]) & sfRedir) {
			tg[x] = canon[tg[x]];
			__builtin_prefetch(&FL(tg[x]));
		}
	for (u32 x = 0; x < nat; x++)
		if (SF(atg[x]) & sfRedir) {
			atg[x] = canon[atg[x]];
			__builtin_prefetch(&FL(atg[x]));
		}
	tscStage[tscW(w)][4] += __rdtsc() - t1;
}
// a function's FuncInfo: where it is, then its bytes
static inline void bFuncInfo(fbatch *b) {
	struct {
		object *o;
		u32 j;
	} fi[floodBatch];
	for (size_t k = 0; k < b->n; k++) {
		fi[k].o = NULL;
		for (u32 a = 0; a < b->it[k].na; a++)
			if (aType(AUX(b->it[k].as, a)) == AuxFuncInfo) {
				u32 c = b->atg.a[b->it[k].a0 + a];
				if (c != 0 && c <= nslots) {
					object *o2 = objOf(c, &fi[k].j);
					if (fi[k].j < o2->nalldef) {
						fi[k].o = o2;
						__builtin_prefetch(o2->b + o2->off[BlkDataIdx] + fi[k].j * 4);
					}
				}
				break;
			}
	}
	for (size_t k = 0; k < b->n; k++)
		if (fi[k].o) {
			bytes d = oData(fi[k].o, fi[k].j);
			__builtin_prefetch(d.p);
			__builtin_prefetch(d.p + 64);
		}
}

static void floodTask(void *ctx UNUSED, int w, long i) {
	size_t lo = (size_t)i * floodGrain, hi = MIN(lo + floodGrain, frontier.n);
	floodWorker *fw = &fws[w];
	fbatch *cur = &fw->batch[0], *next = &fw->batch[1];
	u64 t0 = __rdtsc(), t1;
#define STAGE(k) (t1 = __rdtsc(), tscStage[tscW(w)][k] += t1 - t0, t0 = t1)
	cur->f = frontier.a + lo;
	cur->n = MIN((size_t)floodBatch, hi - lo);
	bIndex(cur);
	STAGE(0);
	bRelocs(cur);
	STAGE(1);
	for (size_t at = lo; at < hi;) {
		size_t nx = at + cur->n;
		next->f = frontier.a + nx;
		next->n = nx < hi ? MIN((size_t)floodBatch, hi - nx) : 0;
		bIndex(next);
		STAGE(0);
		bDecode(cur);
		STAGE(2);
		bRelocs(next);
		STAGE(1);
		bDups(cur, w);
		t0 = __rdtsc();
		bFuncInfo(cur);
		STAGE(5);
		for (size_t k = 0; k < cur->n; k++) process(fw, cur->f[k], cur->it[k].uii, cur->tg.a + cur->it[k].t0, cur->atg.a + cur->it[k].a0);
		STAGE(6);
		tscStage[tscW(w)][7] += cur->n;
		fw->nflooded += cur->n;
		fw->nrelocs += cur->nt;
		fw->nauxs += cur->nat;
		at = nx;
		fbatch *t = cur;
		cur = next;
		next = t;
	}
#undef STAGE
}

static void methodTask(void *ctx UNUSED, int w, long i) {
	size_t hi = MIN((size_t)(i + 1) * methodGrain, markable.n);
	for (size_t k = (size_t)i * methodGrain; k < hi; k++) {
		methodref *m = &markable.a[k];
		if ((reflectSeen && isExported(m->sig.name)) || ssHas(&ifaceMethod, m->sig, false) || ssHas(&genericIfaceMethod, m->sig, true) || flagNoDCE) {
			u32 j, nr;
			object *o = objOf(m->src, &j);
			relocp rs = oRelocs(o, j, &nr);
			for (u32 d = 0; d < 3; d++) {
				u32 g = RREF(o, RELOC(rs, m->r + d));
				if (g != 0) fmark(&fws[w], canonOf(g));
			}
			keepM[k] = false;
		} else {
			keepM[k] = true;
		}
	}
}

static void publishTask(void *ctx UNUSED, int w UNUSED, long ui) {
	unit *u = &units[ui];
	u32 lo = u->o->base + u->lo, hi = u->o->base + u->hi;
	for (u32 g = lo; g < hi; g++) {
		u8 f = FL(g) & (u8)~(stUndef | flRef); // (those two are fl's own: in state, bit 6 is stOverride)
		if (f) state[g] |= f;
	}
}

static int strPtrCmp(const void *a, const void *b) { return strcmpGo(*(const str *)a, *(const str *)b); }

static void deadcode(void) {
	double tStart = nowSeconds(), tFlood = 0, tBig = 0, tMeth = 0;
	long nBig = 0;
	fws = calloc((size_t)nthreads, sizeof(floodWorker));
	for (int i = 0; i < nthreads; i++) fws[i].batch = calloc(2, sizeof(fbatch));
	floodWorker *w0 = &fws[0];
#define ROOT(c)                                    \
	do {                                           \
		u32 c_ = (c);                              \
		if (c_ != 0) fmark(w0, canonOf(c_));         \
	} while (0)
	ROOT(lookupC(flagEntry, 0));
	ROOT(lookupC(flagEntry, 1));
	ROOT(lookupC("runtime.unreachableMethod", 0));
	ROOT(lookupC("runtime.unreachableMethod", 1));
	ROOT(lookupC("runtime.mapinitnoop", 1));
	ROOT(lookupC("runtime.firstmoduledata", 0));
	for (size_t i = 0; i < mainInittasks.n; i++) ROOT(mainInittasks.a[i]);
	for (size_t i = 0; i < runtimeInittasks.n; i++) ROOT(runtimeInittasks.a[i]);
	if (flagNoDCE) {
		for (long ui = 0; ui < nunits; ui++) {
			unit *u = &units[ui];
			for (u32 j = u->lo; j < MIN(u->hi, u->o->nalldef); j++) {
				u32 g = u->o->base + j;
				if (canonOf(g) == g) {
					u8 k = symType(osym(u->o, j));
					if (k != K_Sxxx && k < K_SDWARFCUINFO) ROOT(g);
				}
			}
		}
	}
	ssInit(&ifaceMethod, 1024);
	ssInit(&genericIfaceMethod, 64);
	reflectSeen = flagNoDCE;
	int levels = 0, rounds = 0;
	for (;;) {
		rounds++;
		for (;;) {
			frontier.n = 0;
			for (int i = 0; i < nthreads; i++) {
				for (size_t k = 0; k < fws[i].next.n; k++) VPUSH(frontier, fws[i].next.a[k]);
				fws[i].next.n = 0;
			}
			if (frontier.n == 0) break;
			levels++;
			double tf = nowSeconds();
			par((long)((frontier.n + floodGrain - 1) / floodGrain), floodTask, NULL);
			tFlood += nowSeconds() - tf;
			if (frontier.n >= 4096) tBig += nowSeconds() - tf, nBig += frontier.n;
		}
		for (int i = 0; i < nthreads; i++) {
			floodWorker *w = &fws[i];
			for (size_t k = 0; k < w->ifaceM.n; k++) ssAdd(&ifaceMethod, w->ifaceM.a[k], false);
			for (size_t k = 0; k < w->generic.n; k++) ssAdd(&genericIfaceMethod, (methodsig){w->generic.a[k], 0}, true);
			reflectSeen = reflectSeen || w->reflect;
			for (size_t k = 0; k < w->methods.n; k++) VPUSH(markable, w->methods.a[k]);
			w->ifaceM.n = w->generic.n = w->methods.n = 0;
		}
		// methods of live types that something can now call: an interface with that method is live, or
		// reflection may call any exported method
		keepM = realloc(keepM, MAX(markable.n, 1));
		double tm = nowSeconds();
		par((long)((markable.n + methodGrain - 1) / methodGrain), methodTask, NULL);
		tMeth += nowSeconds() - tm;
		size_t r = 0;
		for (size_t k = 0; k < markable.n; k++)
			if (keepM[k]) markable.a[r++] = markable.a[k];
		markable.n = r;
		bool more = false;
		for (int i = 0; i < nthreads; i++) more = more || fws[i].next.n != 0;
		if (!more) break;
	}
	// the content-addressed symbols it reached are joined now (resolve.c), then state learns what it found
	double th = nowSeconds();
	resolveHashed();
	th = nowSeconds() - th;
	double tp = nowSeconds();
	par(nunits, publishTask, NULL);
	tp = nowSeconds() - tp;
	if (getenv("HLINK_TSC")) fprintf(stderr, "deadcode wall ms: floods %.1f (levels of 4096+ symbols: %.1f for %ld symbols), methods %.1f, hashed join %.1f, publish %.1f, since start %.1f\n", tFlood * 1e3, tBig * 1e3, nBig, tMeth * 1e3, th * 1e3, tp * 1e3, (nowSeconds() - tStart) * 1e3);
	long nlive = 0;
	VEC(str) dump = {0};
	if (flagStats || *flagDumpLive) {
		for (u32 g = 1; g <= nslots; g++) {
			if ((state[g] & stLive) && canonOf(g) == g) {
				nlive++;
				if (*flagDumpLive) VPUSH(dump, symName(g));
			}
		}
	}
	if (*flagDumpLive) {
		qsort(dump.a, dump.n, sizeof(str), strPtrCmp);
		FILE *f = fopen(flagDumpLive, "w");
		if (f) {
			for (size_t i = 0; i < dump.n; i++) {
				fwrite(dump.a[i].p, 1, dump.a[i].n, f);
				fputc('\n', f);
			}
			if (dump.n == 0) fputc('\n', f);
			fclose(f);
		}
		free(dump.a);
	}
	if (getenv("HLINK_TSC")) {
		u64 t[8] = {0};
		for (int i = 0; i < nthreads; i++)
			for (int k = 0; k < 8; k++) t[k] += tscStage[i][k];
		fprintf(stderr, "flood, core-ms: index %.1f, relocs+aux %.1f, decode %.1f, dup ids %.1f, dup flags %.1f, funcinfo %.1f, process %.1f; %llu symbols flooded\n",
		        t[0] / 3.095e6, t[1] / 3.095e6, t[2] / 3.095e6, t[3] / 3.095e6, t[4] / 3.095e6, t[5] / 3.095e6, t[6] / 3.095e6, (unsigned long long)t[7]);
	}
	u64 nfl = 0, nrl = 0, nax = 0;
	for (int i = 0; i < nthreads; i++) {
		nfl += fws[i].nflooded;
		nrl += fws[i].nrelocs;
		nax += fws[i].nauxs;
	}
	lap("deadcode", "%ld symbols live, %d levels in %d rounds, %zu methods pruned, reflection %s; %llu symbols flooded, %llu relocations and %llu aux records read; content-addressed: %ld reached, %ld kept",
	    nlive, levels, rounds, markable.n, reflectSeen ? "true" : "false", (unsigned long long)nfl, (unsigned long long)nrl, (unsigned long long)nax, nHashedReached, nHashedClasses);
}
