// layout.c: where everything goes. Each live definition belongs to a bucket (a part of a section) and an
// alignment class. One parallel pass sums, per unit, the bytes each (bucket, class) needs; prefix sums over the
// units in address order give every unit's block of every bucket its place (inside a block the classes go from
// the largest alignment down, so nothing is wasted on padding); a second parallel pass hands out the
// addresses; a third gives every other slot (duplicates, references) the address of its definition, so that
// applying a relocation is one read of a table indexed by the slot the relocation names.
//
// The image, in address order (file offset = address - imageBase everywhere):
//
//	ELF header, program headers, build id note
//	text            functions; then the FIPS functions
//	rodata          read-only data; FIPS read-only data; strings the linker makes; the GC programs of data and bss
//	pclntab         header, function names, cutab, file names, pc-value tables, functab, funcdata, findfunctab
//	types           the types reflection looks up by string, sorted by it; other type data; itabs
//	(page boundary)
//	noptrdata       build info, moduledata, init task lists; pointer-free data; FIPS
//	data            data with pointers; FIPS
//	bss, noptrbss   (not in the file)

static struct {
	u32 text, etext;
	u32 textFIPS, etextFIPS;
	u32 rodata, erodata;
	u32 rodataFIPS, erodataFIPS;
	u32 gcdata, gcbss; // room for the GC programs
	u32 gcdataMax, gcbssMax;
	u32 pclntab, epclntab;
	u32 pcheader, funcnametab, cutab;
	u32 filetab, pctab, functab;
	u32 ftabStructs; // offset of the first _func in functab
	u32 functabSize;
	u32 gofunc, findfunctab;
	u32 findfuncSize;
	u32 types, etypes;
	u32 typedesclen;
	u32 itabs, eitabs;
	u32 noptrdata, enoptrdata;
	u32 noptrdataFIPS, enoptrdataFIP;
	u32 data, edata;
	u32 dataFIPS, edataFIPS;
	u32 bss, ebss;
	u32 noptrbss, enoptrbss;
	u32 covctrs, ecovctrs;
	u32 end;
	u32 nfunc, nfiles, cutabLen;
	u32 nameSize, pctabSize;
	u32 fileSize;
	VEC(char *) fileNames; // expanded, in filetab order
	vecu32 fileOffs;
	vecu32 cutabData;
} lay;

static u32 symAlign(const u8 *s) {
	u32 a = symAlignField(s);
	if (a != 0) return a;
	u32 siz = symSiz(s);
	a = maxAlign;
	while (a > siz && a > 1) a >>= 1;
	return a;
}

// funcShape reads what a function's pclntab entry needs to know: its FuncInfo, and how many pcdata and
// funcdata slots it has (an inlined-into function has at least the inline-tree ones).
static bytes funcShape(const object *o, u32 j, u32 *npc, u32 *nfd, u32 *ninl) {
	u32 na;
	auxp as = oAuxs(o, j, &na);
	bytes fi = {0, 0};
	*npc = *nfd = *ninl = 0;
	for (u32 i = 0; i < na; i++) {
		auxp a = AUX(as, i);
		switch (aType(a)) {
		case AuxFuncInfo: fi = dataOf(canon[AREF(o, a)]); break;
		case AuxPcdata: (*npc)++; break;
		case AuxFuncdata: (*nfd)++; break;
		}
	}
	if (fi.n == 0) {
		*npc = *nfd = 0;
		return fi;
	}
	u32 inlOff;
	*ninl = fiNumInl(fi, &inlOff);
	if (*ninl > 0) {
		*npc = MAX(*npc, ABI_PCDATA_InlTreeIndex + 1);
		*nfd = MAX(*nfd, ABI_FUNCDATA_InlTree + 1);
	}
	return fi;
}

static int bucketOf(const object *o, const u8 *s, u32 st) {
	switch (symType(s)) {
	case K_STEXT: return bText;
	case K_STEXTFIPS: return bTextFIPS;
	case K_SRODATA:
		if (st & stFuncdata) return bGofunc;
		if (symIsItab(s)) return bItab;
		if (hasPrefix(symNameOf(o, s), "type:")) return symTypelink(s) ? bTypelink : bType;
		return bRodata;
	case K_SRODATAFIPS: return bRodataFIPS;
	case K_SNOPTRDATA:
	case K_SCOVERAGE_AUXVAR: return bNoptrdata;
	case K_SNOPTRDATAFIPS: return bNoptrdataFIPS;
	case K_SDATA: return bData;
	case K_SDATAFIPS: return bDataFIPS;
	case K_SBSS: return bBSS;
	case K_SNOPTRBSS: return bNoptrBSS;
	case K_SCOVERAGE_COUNTER: return bCov;
	}
	return bNone;
}

static inline bool isTextBucket(int b) { return b == bText || b == bTextFIPS; }

// occupied: the bytes a symbol takes in its class's run (its size rounded up to the class's alignment).
static inline u32 occupied(u32 siz, int b, u32 cl) {
	if (isTextBucket(b) && siz < minFunc) siz = minFunc;
	return rnd(siz, 1u << cl);
}

typedef struct {
	u32 gid;
	str s;
} typelink;

// typeString returns a type descriptor's string (what reflect's lookup by string compares).
static str typeString(u32 gid) {
	u32 j, nr;
	object *o = objOf(gid, &j);
	bytes p = oData(o, j);
	relocp rs = oRelocs(o, j, &nr);
	relocp rl = relocAt(rs, nr, 4 * 8 + 8);
	if (!rl) return (str){"", 0};
	bytes d = dataOf(canon[RREF(o, rl)]);
	if (d.n < 2) return (str){"", 0};
	u32 k;
	u64 n = uvarint(d.p + 1, d.n - 1, &k);
	str s = {(const char *)d.p + 1 + k, (u32)n};
	if (p.n > ABI_TFlagOff8 && (p.p[ABI_TFlagOff8] & ABI_TFlagExtraStar) && s.n > 0) {
		s.p++;
		s.n--;
	}
	return s;
}

static inline int cmpTypelink(const typelink *a, const typelink *b) {
	int c = strcmpGo(a->s, b->s);
	if (c != 0) return c;
	return a->gid < b->gid ? -1 : a->gid > b->gid;
}
static int cmpTypelinkQ(const void *a, const void *b) { return cmpTypelink(a, b); }

// ---- sizes
static void sizeUnit(void *ctx UNUSED, int w UNUSED, long ui) {
	unit *u = &units[ui];
	object *o = u->o;
	for (u32 j = u->lo; j < MIN(u->hi, o->nalldef); j++) {
		u32 gid = o->base + j;
		u32 st = state[gid];
		if ((st & (stLive | stPC | stNameNeeded)) == 0 || canon[gid] != gid) continue;
		const u8 *s = osym(o, j);
		if (st & stPC) u->pcSize += oDataSize(o, j);
		int b = bNone;
		// (a pc-value table lives in pctab only, even when -nodce makes it live too: fclink places it in rodata
		// as well and then copies it to pctab at its rodata address, out of bounds)
		if ((st & stLive) && !(st & (stOverride | stPC))) b = bucketOf(o, s, st);
		if (isTextBucket(b)) {
			u32 npc, nfd, ninl;
			funcShape(o, j, &npc, &nfd, &ninl);
			u->nfunc[b - bText]++;
			u->ftabSize += rnd(funcSize + 4 * npc + 4 * nfd, 8);
			u->inlSize += 16 * ninl;
			u->nameSize += symNameLen(s) + 1;
		} else if (st & stNameNeeded) {
			u->nameSize += symNameLen(s) + 1;
		}
		if (b == bNone) continue;
		if (b == bTypelink) {
			VPUSH(u->typelinks, gid);
			state[gid] = st | (u32)bTypelink << stBucketShift;
			continue;
		}
		u32 a = symAlign(s);
		if (isTextBucket(b)) a = MAX(a, funcAlign);
		if (b == bItab) a = 8; // the runtime walks the itabs: no gaps
		u32 cl = (u32)__builtin_ctz(a);
		if ((a & (a - 1)) != 0 || cl >= nClasses) {
			str n = symNameOf(o, s);
			errorf("%.*s: alignment %u", (int)n.n, n.p, a);
			continue;
		}
		state[gid] = st | (u32)b << stBucketShift | cl << stClassShft;
		if (isTextBucket(b)) u->nfuncCl[b - bText][cl]++;
		u->csize[b][cl] += occupied(symSiz(s), b, cl);
	}
}

static typelink *tls;
static size_t ntls;
#define tgrain 512
static void typeStringTask(void *ctx UNUSED, int w UNUSED, long i) {
	for (size_t k = (size_t)i * tgrain; k < MIN((size_t)(i + 1) * tgrain, ntls); k++) tls[k].s = typeString(tls[k].gid);
}

// the sort of the typelinks: runs sorted in parallel, then rounds of parallel pairwise merges
static typelink *tlsTmp;
static size_t sortRun;
static void sortRunTask(void *ctx UNUSED, int w UNUSED, long i) {
	size_t lo = (size_t)i * sortRun, hi = MIN(lo + sortRun, ntls);
	qsort(tls + lo, hi - lo, sizeof(typelink), cmpTypelinkQ);
}
static void mergeTask(void *ctx, int w UNUSED, long i) {
	size_t width = *(size_t *)ctx;
	size_t lo = (size_t)i * 2 * width, mid = MIN(lo + width, ntls), hi = MIN(lo + 2 * width, ntls);
	size_t a = lo, b = mid, k = lo;
	while (a < mid && b < hi) tlsTmp[k++] = cmpTypelink(&tls[b], &tls[a]) < 0 ? tls[b++] : tls[a++];
	while (a < mid) tlsTmp[k++] = tls[a++];
	while (b < hi) tlsTmp[k++] = tls[b++];
}
static void sortTypelinks(void) {
	if (ntls < 2) return;
	size_t nrun = (size_t)nthreads * 4;
	sortRun = MAX((ntls + nrun - 1) / nrun, 2048);
	par((long)((ntls + sortRun - 1) / sortRun), sortRunTask, NULL);
	tlsTmp = malloc(ntls * sizeof(typelink));
	for (size_t width = sortRun; width < ntls; width *= 2) {
		par((long)((ntls + 2 * width - 1) / (2 * width)), mergeTask, &width);
		typelink *t = tls;
		tls = tlsTmp;
		tlsTmp = t;
	}
	free(tlsTmp);
}

static char *expandFile(str name);

// ---- addresses of the units' blocks, in address order
static u32 va;
static void bucket(int b, u32 minAlign, u32 *start, u32 *end) {
	va = rnd(va, minAlign);
	if (start) *start = va;
	for (long ui = 0; ui < nunits; ui++) {
		unit *u = &units[ui];
		u32 *cs = u->csize[b];
		int top = -1;
		for (int cl = nClasses - 1; cl >= 0; cl--)
			if (cs[cl] != 0) {
				top = cl;
				break;
			}
		if (top >= 0) va = rnd(va, MAX(minAlign, 1u << top));
		for (int cl = nClasses - 1; cl >= 0; cl--) { // (empty classes too: zero-sized symbols need an address)
			u->cbase[b][cl] = va;
			va += cs[cl];
		}
	}
	if (end) *end = va;
}

// A special is a symbol whose content the linker writes: one it makes, or one an object defines and the linker
// overrides (runtime.firstmoduledata, ...).
enum {
	spBytes,       // copy data
	spStrHdr,      // a string header: the address of sym, then n
	spModuledata,  //
	spAddr,        // one word: the address of sym
	spTaskList,    // the addresses of the tasks
	spRtInittasks, // a slice header of the runtime's task list (sym), n
	spTextsect,    // runtime.textsectionmap
	spByte1,       // one byte: 1
	spFipsinfo,
};
typedef struct {
	u32 gid;
	u32 size, align;
	int kind;
	const u8 *data;
	u32 sym, n;
	const u32 *tasks;
} special;
typedef VEC(special) vecspecial;
static vecspecial roSpecials, dataSpecials;

static void place(special *list, size_t n) {
	for (size_t i = 0; i < n; i++) {
		va = rnd(va, list[i].align);
		addr[list[i].gid] = va;
		va += list[i].size;
	}
}

static void addrUnit(void *ctx UNUSED, int w UNUSED, long ui) {
	unit *u = &units[ui];
	object *o = u->o;
	u32 cur[nBuckets][nClasses];
	memcpy(cur, u->cbase, sizeof cur);
	u32 pc = u->pcBase, name = u->nameBase;
	for (u32 j = u->lo; j < MIN(u->hi, o->nalldef); j++) {
		u32 gid = o->base + j;
		u32 st = state[gid];
		if ((st & (stLive | stPC | stNameNeeded)) == 0 || canon[gid] != gid) continue;
		const u8 *s = osym(o, j);
		if (st & stPC) {
			u32 n = oDataSize(o, j);
			if (n != 0) {
				addr[gid] = pc;
				pc += n;
			}
		}
		int b = (int)(st >> stBucketShift & 31);
		if (isTextBucket(b) || (st & stNameNeeded)) {
			aux[gid] = name;
			name += symNameLen(s) + 1;
		}
		if (b == bNone || b == bTypelink) continue;
		u32 cl = st >> stClassShft & 15;
		addr[gid] = cur[b][cl];
		cur[b][cl] += occupied(symSiz(s), b, cl);
	}
}

static void otherSlots(void *ctx UNUSED, int w UNUSED, long ui) {
	unit *u = &units[ui];
	u32 base = u->o->base;
	for (u32 g = base + u->lo; g < base + u->hi; g++) {
		u32 c = canon[g];
		if (c != g) addr[g] = addr[c];
	}
}

static void makeSpecials(void);
static void defineSyms(void);

static double mb(u32 n) { return (double)n / (1 << 20); }

static void layout(void) {
	makeSpecials();
	par(nunits, sizeUnit, NULL);

	// ---- the typelink types, sorted by type string (reflect binary-searches them)
	for (long ui = 0; ui < nunits; ui++) ntls += units[ui].typelinks.n;
	tls = malloc(MAX(ntls, 1) * sizeof(typelink));
	size_t k = 0;
	for (long ui = 0; ui < nunits; ui++)
		for (size_t i = 0; i < units[ui].typelinks.n; i++) tls[k++] = (typelink){units[ui].typelinks.a[i], {0, 0}};
	par((long)((ntls + tgrain - 1) / tgrain), typeStringTask, NULL);
	sortTypelinks();

	// ---- pclntab tables that are per object: the file table
	for (long ui = 0; ui < nunits; ui++) units[ui].o->nfunc += units[ui].nfunc[0] + units[ui].nfunc[1];
	strmap fileOff;
	smInit(&fileOff, 4096);
	for (int i = 0; i < nobjs; i++) {
		object *o = objs[i];
		if (o->nfunc == 0) continue;
		o->cuOff = (u32)lay.cutabData.n;
		for (u32 f = 0, n = oNFile(o); f < n; f++) {
			str name = oFile(o, f);
			bool added;
			long *off = smPut(&fileOff, name, lay.fileSize, &added);
			if (added) {
				char *x = expandFile(name);
				VPUSH(lay.fileNames, x);
				VPUSH(lay.fileOffs, lay.fileSize);
				lay.fileSize += (u32)strlen(x) + 1;
			}
			VPUSH(lay.cutabData, (u32)*off);
		}
	}
	lay.nfiles = (u32)lay.fileNames.n;

	va = imageBase + headerSize;
	bucket(bText, funcAlign, &lay.text, NULL);
	bucket(bTextFIPS, funcAlign, &lay.textFIPS, &lay.etextFIPS);
	lay.etext = va;

	va = rnd(va + 1, 4096);
	bucket(bRodata, 8, &lay.rodata, NULL);
	bucket(bRodataFIPS, 32, &lay.rodataFIPS, &lay.erodataFIPS);
	place(roSpecials.a, roSpecials.n);
	u32 dataMax = 0, bssMax = 0;
	for (long ui = 0; ui < nunits; ui++) {
		unit *u = &units[ui];
		for (int cl = 0; cl < nClasses; cl++) {
			dataMax += u->csize[bData][cl] + u->csize[bDataFIPS][cl];
			bssMax += u->csize[bBSS][cl];
		}
		dataMax += 128;
		bssMax += 64;
	}
	lay.gcdataMax = dataMax / 8 / 7 + 64; // a literal run spends 8 bits on 7
	lay.gcbssMax = bssMax / 8 / 7 + 64;
	lay.gcdata = va;
	va += lay.gcdataMax;
	lay.gcbss = va;
	va += lay.gcbssMax;
	lay.erodata = va;

	lay.nameSize = 1; // name offset 0 means "no name"
	for (long ui = 0; ui < nunits; ui++) {
		unit *u = &units[ui];
		u->funcBase[0] = lay.nfunc; // (the function table is in address order: the FIPS functions come last)
		lay.nfunc += u->nfunc[0];
		u->nameBase = lay.nameSize;
		lay.nameSize += u->nameSize;
		u->pcBase = 1 + lay.pctabSize; // offset 0 means "no table"
		lay.pctabSize += u->pcSize;
		u->ftabBase = lay.functabSize;
		lay.functabSize += u->ftabSize;
	}
	for (long ui = 0; ui < nunits; ui++) {
		units[ui].funcBase[1] = lay.nfunc;
		lay.nfunc += units[ui].nfunc[1];
	}
	lay.pctabSize++;
	va = rnd(va, 32);
	lay.pclntab = lay.pcheader = va;
	va += 8 + 8 * 8;
	lay.funcnametab = va;
	va = rnd(va + lay.nameSize, 4);
	lay.cutab = va;
	lay.cutabLen = (u32)lay.cutabData.n;
	va += 4 * lay.cutabLen;
	lay.filetab = va;
	va += lay.fileSize;
	lay.pctab = va;
	va = rnd(va + lay.pctabSize, 8);
	lay.functab = va;
	lay.ftabStructs = rnd(lay.nfunc * 8 + 4, 8);
	lay.functabSize += lay.ftabStructs;
	va += lay.functabSize;
	va = rnd(va, 32);
	lay.gofunc = va;
	bucket(bGofunc, 8, NULL, NULL);
	va = rnd(va, 4);
	for (long ui = 0; ui < nunits; ui++) {
		units[ui].inlBase = va;
		va += units[ui].inlSize;
	}
	va = rnd(va, 4);
	lay.findfunctab = va;
	u32 textLen = lay.etext - lay.text;
	lay.findfuncSize = 4 * ((textLen + ABI_FuncTabBucketSize - 1) / ABI_FuncTabBucketSize) +
	                   (textLen + ABI_FuncTabBucketSize / 16 - 1) / (ABI_FuncTabBucketSize / 16);
	va += lay.findfuncSize;
	lay.epclntab = va;

	va = rnd(va, 32);
	lay.types = va;
	va += 8; // so that no type has offset 0
	for (size_t i = 0; i < ntls; i++) {
		u32 j;
		object *o = objOf(tls[i].gid, &j);
		const u8 *s = osym(o, j);
		va = rnd(va, MAX(8, symAlign(s)));
		addr[tls[i].gid] = va;
		va += symSiz(s);
	}
	lay.typedesclen = va - lay.types;
	bucket(bType, 8, NULL, NULL);
	bucket(bItab, 8, &lay.itabs, &lay.eitabs);
	lay.etypes = va;

	va = rnd(va, 4096);
	lay.noptrdata = va;
	place(dataSpecials.a, dataSpecials.n);
	bucket(bNoptrdata, 8, NULL, NULL);
	bucket(bNoptrdataFIPS, 32, &lay.noptrdataFIPS, &lay.enoptrdataFIP);
	lay.enoptrdata = va;
	bucket(bData, 64, &lay.data, NULL); // 64 bytes are one byte of GC bitmap: units write disjoint bytes
	bucket(bDataFIPS, 64, &lay.dataFIPS, &lay.edataFIPS);
	va = rnd(va, 8);
	lay.edata = va;
	bucket(bBSS, 64, &lay.bss, NULL);
	va = rnd(va, 8);
	lay.ebss = va;
	bucket(bNoptrBSS, 32, &lay.noptrbss, NULL);
	bucket(bCov, 8, &lay.covctrs, &lay.ecovctrs);
	lay.enoptrbss = va;
	lay.end = va;
	if (va >= 1u << 31) fatalf("image too large: %#x", va);
	if (lay.nfunc == 0) fatalf("no functions are reachable: is the entry symbol %s defined?", flagEntry);

	// ---- every live definition's address, name offset, pc-value table offset
	par(nunits, addrUnit, NULL);
	defineSyms();
	// ---- every other slot: the address of what it resolves to
	par(nunits, otherSlots, NULL);
	lap("layout", "%u functions, text %.1f MB, rodata %.1f, pclntab %.1f, types %.1f (%zu sorted), data %.1f, bss %.1f", lay.nfunc,
	    mb(lay.etext - lay.text), mb(lay.erodata - lay.rodata), mb(lay.epclntab - lay.pclntab), mb(lay.etypes - lay.types), ntls,
	    mb(lay.edata - lay.noptrdata), mb(lay.end - lay.edata));
}
