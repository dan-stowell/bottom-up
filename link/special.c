// special.c: what the linker itself contributes: runtime.firstmoduledata, the init task lists, the string
// variables it sets, go:buildinfo, go:fipsinfo, the section boundary symbols, the fixed tables of pclntab,
// findfunctab, the GC programs of the data and bss sections; and the ELF headers.

static u32 modInittasks, textsect, fipsinfo, buildinfoSym;

static void addSpecial(vecspecial *list, u32 gid, u32 size, u32 align, int kind, const u8 *data, u32 sym, u32 n, const u32 *tasks) {
	special sp = {gid, size, align, kind, data, sym, n, tasks};
	VPUSH(*list, sp);
	state[gid] |= stLive | stOverride;
}

static char *expandFile(str name) {
	if (hasPrefix(name, "gofile..")) {
		name.p += 8;
		name.n -= 8;
	}
	const char *goroot = getenv("GOROOT"); // buildcfg.GOROOT
	const u32 n = 7;                        // len("$GOROOT")
	if (name.n >= n + 1 && memcmp(name.p, "$GOROOT", n) == 0 && (name.p[n] == '/' || name.p[n] == '\\') && goroot && *goroot) {
		// filepath.ToSlash(filepath.Join(GOROOT, rest)): join and clean
		char *joined;
		if (asprintf(&joined, "%s/%.*s", goroot, (int)(name.n - n), name.p + n) < 0) fatalf("out of memory");
		bool abs = joined[0] == '/';
		char *c = abs ? absClean(joined) : NULL;
		if (c) {
			free(joined);
			return c;
		}
		return joined;
	}
	return strdupStr(name);
}

static int strdefCmp(const void *a, const void *b) { return strcmp(((const strdef *)a)->name, ((const strdef *)b)->name); }

static void fipsBounds(u32 fb[8]) {
	u32 v[8] = {lay.textFIPS, lay.etextFIPS, lay.rodataFIPS, lay.erodataFIPS, lay.noptrdataFIPS, lay.enoptrdataFIP, lay.dataFIPS, lay.edataFIPS};
	memcpy(fb, v, sizeof v);
}

static void appendUvarint(vecu8 *b, u64 x) {
	while (x >= 0x80) {
		VPUSH(*b, (u8)(x | 0x80));
		x >>= 7;
	}
	VPUSH(*b, (u8)x);
}

static void makeSpecials(void) {
	// string variables: -X, runtime.buildVersion, runtime.modinfo, runtime.defaultGOROOT
	strdef *names = malloc(MAX(strdata.n, 1) * sizeof(strdef));
	memcpy(names, strdata.a, strdata.n * sizeof(strdef));
	qsort(names, strdata.n, sizeof(strdef), strdefCmp);
	for (size_t i = 0; i < strdata.n; i++) {
		u32 v = lookupC(names[i].name, 0);
		if (!isLive(v) || !isDef(v)) continue;
		str val = names[i].val;
		char *sn;
		if (asprintf(&sn, "%s.str", names[i].name) < 0) fatalf("out of memory");
		u32 s = synth(sn);
		free(sn);
		addSpecial(&roSpecials, s, val.n, 1, spBytes, (const u8 *)val.p, 0, 0, NULL);
		u32 j;
		object *o = objOf(v, &j);
		addSpecial(&dataSpecials, v, MAX(16, symSiz(osym(o, j))), 8, spStrHdr, NULL, s, val.n, NULL);
	}

	// go:buildinfo, which `go version` reads
	vecu8 bi = {0};
	const char *magic = "\xff Go buildinf:\x08\x02";
	for (size_t i = 0; i < 16; i++) VPUSH(bi, (u8)magic[i]);
	while (bi.n < 32) VPUSH(bi, 0);
	str bv = getStrdata("runtime.buildVersion"), mi = getStrdata("runtime.modinfo");
	appendUvarint(&bi, bv.n);
	for (u32 i = 0; i < bv.n; i++) VPUSH(bi, (u8)bv.p[i]);
	appendUvarint(&bi, mi.n);
	for (u32 i = 0; i < mi.n; i++) VPUSH(bi, (u8)mi.p[i]);
	while (bi.n % 16 != 0) VPUSH(bi, 0);
	// (first in the data segment: the specials are placed in order)
	buildinfoSym = synth("go:buildinfo");
	special bsp = {buildinfoSym, (u32)bi.n, 16, spBytes, bi.a, 0, 0, NULL};
	VPUSH(dataSpecials, bsp);
	memmove(dataSpecials.a + 1, dataSpecials.a, (dataSpecials.n - 1) * sizeof(special));
	dataSpecials.a[0] = bsp;

	u32 md = lookupC("runtime.firstmoduledata", 0);
	if (!isDef(md)) fatalf("runtime.firstmoduledata is not defined: is the runtime in the importcfg?");
	u32 j;
	object *o = objOf(md, &j);
	addSpecial(&dataSpecials, md, symSiz(osym(o, j)), 8, spModuledata, NULL, 0, 0, NULL);
	u32 g = lookupC("runtime.lastmoduledatap", 0);
	if (isLive(g)) addSpecial(&dataSpecials, g, 8, 8, spAddr, NULL, md, 0, NULL);
	u32 mainTasks = synth("go:main.inittasks");
	addSpecial(&dataSpecials, mainTasks, (u32)(8 * mainInittasks.n), 8, spTaskList, NULL, 0, (u32)mainInittasks.n, mainInittasks.a);
	g = lookupC("runtime.runtime_inittasks", 0);
	if (isLive(g)) {
		u32 rt = synth("go:runtime.inittasks");
		addSpecial(&dataSpecials, rt, (u32)(8 * runtimeInittasks.n), 8, spTaskList, NULL, 0, (u32)runtimeInittasks.n, runtimeInittasks.a);
		addSpecial(&dataSpecials, g, 24, 8, spRtInittasks, NULL, rt, (u32)runtimeInittasks.n, NULL);
	}
	modInittasks = mainTasks;
	textsect = synth("runtime.textsectionmap");
	addSpecial(&roSpecials, textsect, 24, 8, spTextsect, NULL, 0, 0, NULL);
	// runtime.memProfileInternal not linked in: the runtime need not collect memory profiles
	u32 mp = lookupC("runtime.memProfileInternal", 1);
	if (mp != 0 && !isLive(mp)) {
		g = lookupC("runtime.disableMemoryProfiling", 0);
		if (isLive(g)) addSpecial(&dataSpecials, g, 1, 1, spByte1, NULL, 0, 0, NULL);
	}
	// go:fipsinfo (crypto/internal/fips140/check.Linkinfo): magic, the hash of the FIPS sections, their bounds.
	// The check package defines it (a linknamed variable); the linker writes its content, as the stock linker
	// does (ld.loadfips). fclink skipped it when it was defined, which is always: with GODEBUG=fips140=on the
	// program then died with "no verification checksum found".
	g = lookupC("go:fipsinfo", 0);
	if (isLive(g)) {
		fipsinfo = g;
		addSpecial(&dataSpecials, g, 16 + 32 + 8 + 8 * 8, 8, spFipsinfo, NULL, 0, 0, NULL);
	}
}

// boundaries: the symbols that name places in the image, and their addresses
typedef struct {
	const char *name;
	u32 a;
} boundary;
#define nBoundaries 40
static void boundaries(boundary def[nBoundaries]) {
	u32 fb[8];
	fipsBounds(fb);
	boundary d[nBoundaries] = {
		{"runtime.text", lay.text}, {"runtime.etext", lay.etext}, {"runtime.rodata", lay.rodata}, {"runtime.erodata", lay.erodata},
		{"runtime.types", lay.types}, {"runtime.etypes", lay.etypes}, {"runtime.noptrdata", lay.noptrdata},
		{"runtime.enoptrdata", lay.enoptrdata}, {"runtime.data", lay.data}, {"runtime.edata", lay.edata}, {"runtime.bss", lay.bss},
		{"runtime.ebss", lay.ebss}, {"runtime.noptrbss", lay.noptrbss}, {"runtime.enoptrbss", lay.enoptrbss},
		{"runtime.covctrs", lay.covctrs}, {"runtime.ecovctrs", lay.ecovctrs}, {"runtime.end", lay.end},
		{"runtime.gcdata", lay.gcdata}, {"runtime.gcbss", lay.gcbss}, {"runtime.egcdata", lay.gcdata + lay.gcdataMax},
		{"runtime.egcbss", lay.gcbss + lay.gcbssMax}, {"runtime.pclntab", lay.pclntab}, {"runtime.epclntab", lay.epclntab},
		{"runtime.pcheader", lay.pcheader}, {"runtime.funcnametab", lay.funcnametab}, {"runtime.cutab", lay.cutab},
		{"runtime.filetab", lay.filetab}, {"runtime.pctab", lay.pctab}, {"runtime.functab", lay.functab},
		{"runtime.findfunctab", lay.findfunctab}, {"go:func.*", lay.gofunc}, {"runtime.gcmask.*", lay.noptrbss},
		{"go:textfipsstart", fb[0]}, {"go:textfipsend", fb[1]}, {"go:rodatafipsstart", fb[2]}, {"go:rodatafipsend", fb[3]},
		{"go:noptrdatafipsstart", fb[4]}, {"go:noptrdatafipsend", fb[5]}, {"go:datafipsstart", fb[6]}, {"go:datafipsend", fb[7]},
	};
	memcpy(def, d, sizeof d);
}

// defineSyms gives the symbols that name places in the image their addresses.
static void defineSyms(void) {
	boundary def[nBoundaries];
	boundaries(def);
	for (size_t i = 0; i < nBoundaries; i++) {
		u32 g = lookupC(def[i].name, 0);
		if (g != 0 && !isDef(g)) addr[g] = def[i].a;
	}
}

static void fillModuledata(u8 *p) {
	size_t off = 0;
#define WORD(v) (st64(p + off, (u64)(v)), off += 8)
#define SLICE(a, n) (WORD(a), WORD(n), WORD(n))
	WORD(lay.pcheader);
	SLICE(lay.funcnametab, lay.nameSize);
	SLICE(lay.cutab, lay.cutabLen);
	SLICE(lay.filetab, lay.pctab - lay.filetab);
	SLICE(lay.pctab, lay.pctabSize);
	SLICE(lay.functab, lay.functabSize);
	SLICE(lay.functab, lay.nfunc + 1);
	WORD(lay.findfunctab);
	u8 *ft = out + (lay.functab - imageBase);
	WORD(lay.text + ld32(ft));                 // minpc
	WORD(lay.text + ld32(ft + 8 * lay.nfunc)); // maxpc
	u32 v[] = {lay.text, lay.etext, lay.noptrdata, lay.enoptrdata, lay.data, lay.edata, lay.bss, lay.ebss,
		lay.noptrbss, lay.enoptrbss, lay.covctrs, lay.ecovctrs, lay.end, lay.gcdata, lay.gcbss, lay.types};
	for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) WORD(v[i]);
	WORD(lay.typedesclen);
	WORD(lay.etypes);
	WORD(lay.itabs - lay.types);
	WORD(lay.eitabs - lay.itabs);
	WORD(lay.rodata);
	WORD(lay.gofunc);
	WORD(lay.epclntab);
	SLICE(addr[textsect], 1);
	SLICE(0, 0); // ptab
	WORD(0);     // pluginpath
	WORD(0);
	SLICE(0, 0); // pkghashes
	if (mainInittasks.n != 0) SLICE(addr[modInittasks], (u32)mainInittasks.n);
	else SLICE(0, 0);
	WORD(0); // modulename
	WORD(0);
	SLICE(0, 0); // modulehashes
	p[off] = 1;  // hasmain
#undef WORD
#undef SLICE
}

// ---- cmd/internal/gcprog.Writer
#define progMaxLiteral 127
typedef struct {
	u8 *room;
	size_t cap, n; // bytes written (may exceed cap: counted, not stored)
	i64 index;
	u8 b[progMaxLiteral];
	int nb;
} gcprogW;
static void gpByte(gcprogW *w, u8 x) {
	if (w->n < w->cap) w->room[w->n] = x;
	w->n++;
}
static void gpVarint(gcprogW *w, i64 x) {
	if (x < 0) fatalf("gcprog: negative varint");
	while (x >= 0x80) {
		gpByte(w, (u8)(0x80 | x));
		x >>= 7;
	}
	gpByte(w, (u8)x);
}
static void gpFlushlit(gcprogW *w) {
	if (w->nb == 0) return;
	gpByte(w, (u8)w->nb);
	u8 bits = 0;
	for (int i = 0; i < w->nb; i++) {
		bits |= (u8)(w->b[i] << (i % 8));
		if ((i + 1) % 8 == 0) {
			gpByte(w, bits);
			bits = 0;
		}
	}
	if (w->nb % 8 != 0) gpByte(w, bits);
	w->nb = 0;
}
static void gpLit(gcprogW *w, u8 x) {
	if (w->nb == progMaxLiteral) gpFlushlit(w);
	w->b[w->nb++] = x;
	w->index++;
}
static void gpRepeat(gcprogW *w, i64 n, i64 c) {
	if (n == 0 || c == 0) return;
	gpFlushlit(w);
	if (n < 128) {
		gpByte(w, (u8)(0x80 | n));
	} else {
		gpByte(w, 0x80);
		gpVarint(w, n);
	}
	gpVarint(w, c);
	w->index += n * c;
}
static void gpZeroUntil(gcprogW *w, i64 index) {
	if (index < w->index) fatalf("gcprog: invalid Advance index");
	i64 skip = index - w->index;
	if (skip == 0) return;
	if (skip < 4 * 8) {
		for (i64 i = 0; i < skip; i++) gpLit(w, 0);
		return;
	}
	gpLit(w, 0);
	gpFlushlit(w);
	gpRepeat(w, 1, skip - 1);
}
static void gpPtr(gcprogW *w, i64 index) {
	if (index < w->index) fatalf("gcprog: invalid Ptr index");
	gpZeroUntil(w, index);
	gpLit(w, 1);
}
static void gpEnd(gcprogW *w) {
	gpFlushlit(w);
	gpByte(w, 0);
}

static void gcProgram(const u8 *bm, i64 nwords, u8 *room, size_t cap) {
	gcprogW w = {room, cap, 0, 0, {0}, 0};
	for (i64 i = 0; i < nwords; i++) {
		if (bm[i >> 3] == 0) {
			i |= 7;
			continue;
		}
		if ((bm[i >> 3] >> (i & 7)) & 1) gpPtr(&w, i);
	}
	gpZeroUntil(&w, nwords);
	gpEnd(&w);
	if (w.n > cap) errorf("GC program of %zu bytes does not fit the %zu reserved", w.n, cap);
}

// ---- SHA-256 and HMAC-SHA256
typedef struct {
	u32 h[8];
	u8 buf[64];
	size_t nbuf;
	u64 len;
} sha256;
static const u32 sha256K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
	0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
	0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
	0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
	0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static inline u32 ror32(u32 x, int n) { return x >> n | x << (32 - n); }
static void shaBlock(sha256 *s, const u8 *p) {
	u32 w[64];
	for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		u32 s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
	for (int i = 0; i < 64; i++) {
		u32 t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + sha256K[i] + w[i];
		u32 t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
	s->h[5] += f;
	s->h[6] += g;
	s->h[7] += h;
}
static void shaInit(sha256 *s) {
	static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(s->h, iv, sizeof iv);
	s->nbuf = 0;
	s->len = 0;
}
static void shaWrite(sha256 *s, const u8 *p, size_t n) {
	s->len += n;
	if (s->nbuf) {
		size_t k = MIN(n, 64 - s->nbuf);
		memcpy(s->buf + s->nbuf, p, k);
		s->nbuf += k;
		p += k;
		n -= k;
		if (s->nbuf == 64) {
			shaBlock(s, s->buf);
			s->nbuf = 0;
		}
	}
	while (n >= 64) {
		shaBlock(s, p);
		p += 64;
		n -= 64;
	}
	if (n) {
		memcpy(s->buf, p, n);
		s->nbuf = n;
	}
}
static void shaSum(sha256 *s, u8 out32[32]) {
	u64 bits = s->len * 8;
	u8 pad = 0x80;
	shaWrite(s, &pad, 1);
	u8 z = 0;
	while (s->nbuf != 56) shaWrite(s, &z, 1);
	u8 l[8];
	for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8 * i));
	shaWrite(s, l, 8);
	for (int i = 0; i < 8; i++) {
		out32[4 * i] = (u8)(s->h[i] >> 24);
		out32[4 * i + 1] = (u8)(s->h[i] >> 16);
		out32[4 * i + 2] = (u8)(s->h[i] >> 8);
		out32[4 * i + 3] = (u8)s->h[i];
	}
}
// HMAC-SHA256 with a key of at most 64 bytes
typedef struct {
	sha256 inner, outer;
} hmac256;
static void hmacInit(hmac256 *m, const u8 *key, size_t klen) {
	u8 ipad[64] = {0}, opad[64] = {0};
	memcpy(ipad, key, klen);
	memcpy(opad, key, klen);
	for (int i = 0; i < 64; i++) {
		ipad[i] ^= 0x36;
		opad[i] ^= 0x5c;
	}
	shaInit(&m->inner);
	shaInit(&m->outer);
	shaWrite(&m->inner, ipad, 64);
	shaWrite(&m->outer, opad, 64);
}
static void hmacSum(hmac256 *m, u8 out32[32]) {
	u8 in[32];
	shaSum(&m->inner, in);
	shaWrite(&m->outer, in, 32);
	shaSum(&m->outer, out32);
}

static void writeELF(void);

static vecu32 ffIdx; // (findfunctab) per sub-bucket: the index of its first function
#define ffGrain 4096
static void findfuncTask(void *ctx UNUSED, int w UNUSED, long c) {
	const u32 sub = ABI_FuncTabBucketSize / 16;
	u8 *ftab = out + (lay.functab - imageBase);
	u32 nsub = (u32)ffIdx.n;
	for (u32 i = (u32)c * ffGrain; i < MIN((u32)(c + 1) * ffGrain, lay.nfunc); i++) {
		// function i covers [p,q): it is the first function of every sub-bucket that starts in there
		u32 p = ld32(ftab + 8 * i), q = ld32(ftab + 8 * i + 8);
		for (u32 k = (p + sub - 1) / sub; k * sub < q && k < nsub; k++) ffIdx.a[k] = i;
	}
}

static void fillSpecial(const special *sp) {
	u8 *p = out + (addr[sp->gid] - imageBase);
	switch (sp->kind) {
	case spBytes: memcpy(p, sp->data, sp->size); break;
	case spStrHdr:
		st64(p, addr[sp->sym]);
		st64(p + 8, sp->n);
		break;
	case spModuledata: fillModuledata(p); break;
	case spAddr: st64(p, addr[sp->sym]); break;
	case spTaskList:
		for (u32 i = 0; i < sp->n; i++) st64(p + 8 * i, addr[sp->tasks[i]]);
		break;
	case spRtInittasks:
		st64(p, addr[sp->sym]);
		st64(p + 8, sp->n);
		st64(p + 16, sp->n);
		break;
	case spTextsect:
		st64(p + 8, lay.etext - lay.text);
		st64(p + 16, lay.text);
		break;
	case spByte1: p[0] = 1; break;
	case spFipsinfo: {
		memcpy(p, "\xff Go fipsinfo \xff\x00", 16);
		st64(p + 48, addr[sp->gid]);
		u32 fb[8];
		fipsBounds(fb);
		for (int i = 0; i < 8; i++) st64(p + 56 + 8 * i, fb[i]);
		break;
	}
	}
}

static void finish(void) {
#define AT(a) (out + ((a) - imageBase))
	// ---- pclntab: header, cutab, file names; the end of the pc -> function table
	u8 *h = AT(lay.pcheader);
	st32(h, ABI_CurrentPCLnTabMagic);
	h[6] = 1; // minLC
	h[7] = 8; // ptrSize
	st64(h + 8, lay.nfunc);
	st64(h + 16, lay.nfiles);
	u32 hv[] = {lay.funcnametab, lay.cutab, lay.filetab, lay.pctab, lay.functab};
	for (int i = 0; i < 5; i++) st64(h + 32 + 8 * i, hv[i] - lay.pcheader);
	u8 *ct = AT(lay.cutab);
	for (size_t i = 0; i < lay.cutabData.n; i++) st32(ct + 4 * i, lay.cutabData.a[i]);
	u8 *ftb = AT(lay.filetab);
	for (size_t i = 0; i < lay.fileNames.n; i++) memcpy(ftb + lay.fileOffs.a[i], lay.fileNames.a[i], strlen(lay.fileNames.a[i]));
	// the last function's end: its entry offset plus its size, which is the address after the last
	// instruction of the text; every function's pad is INT3, so the end of the last block will do
	u8 *ftab = AT(lay.functab);
	u32 lastEntry = ld32(ftab + 8 * (lay.nfunc - 1));
	u32 end = lay.etext - lay.text;
	if (end <= lastEntry) end = lastEntry + 1;
	st32(ftab + 8 * lay.nfunc, end);

	// ---- findfunctab (ld.(*Link).findfunctab): per 4096-byte bucket of text the index of its first function,
	// and per 256-byte sub-bucket the distance from that
	const u32 sub = ABI_FuncTabBucketSize / 16;
	u32 textLen = lay.etext - lay.text;
	u32 nsub = (textLen + sub - 1) / sub;
	u32 nbuckets = (textLen + ABI_FuncTabBucketSize - 1) / ABI_FuncTabBucketSize;
	VRESERVE(ffIdx, MAX(nsub, 1));
	memset(ffIdx.a, 0, MAX(nsub, 1) * sizeof(u32));
	ffIdx.n = nsub;
	par((long)((lay.nfunc + ffGrain - 1) / ffGrain), findfuncTask, NULL);
	u8 *fft = AT(lay.findfunctab);
	for (u32 b = 0; b < nbuckets; b++) {
		i32 base = (i32)ffIdx.a[b * 16];
		st32(fft + 20 * b, (u32)base);
		for (u32 k = 0; k < 16 && b * 16 + k < nsub; k++) {
			i32 d = (i32)ffIdx.a[b * 16 + k] - base;
			if (d < 0 || d >= 256) errorf("findfunctab: bucket %u sub-bucket %u: function index %d from base %d", b, k, (i32)ffIdx.a[b * 16 + k], base);
			fft[20 * b + 4 + k] = (u8)d;
		}
	}

	// ---- the GC programs of data and bss
	gcProgram(gcDataBit, (i64)(lay.edata - lay.data) / 8, AT(lay.gcdata), lay.gcdataMax);
	gcProgram(gcBssBit, (i64)(lay.ebss - lay.bss) / 8, AT(lay.gcbss), lay.gcbssMax);

	for (size_t i = 0; i < roSpecials.n; i++)
		if (roSpecials.a[i].size != 0) fillSpecial(&roSpecials.a[i]);
	for (size_t i = 0; i < dataSpecials.n; i++)
		if (dataSpecials.a[i].size != 0) fillSpecial(&dataSpecials.a[i]);
	if (fipsinfo != 0) {
		// the hash the FIPS self-check verifies at start: HMAC-SHA256 with a zero key over the four sections
		hmac256 hm;
		u8 key[32] = {0};
		hmacInit(&hm, key, 32);
		const char *hdr = "go fips object v1\n";
		shaWrite(&hm.inner, (const u8 *)hdr, strlen(hdr));
		u32 fb[8];
		fipsBounds(fb);
		for (int i = 0; i < 8; i += 2) {
			u64 n = fb[i + 1] - fb[i];
			u8 nb[8];
			for (int k = 0; k < 8; k++) nb[k] = (u8)(n >> (56 - 8 * k));
			shaWrite(&hm.inner, nb, 8);
			shaWrite(&hm.inner, AT(fb[i]), n);
		}
		hmacSum(&hm, AT(addr[fipsinfo]) + 16);
	}
	writeELF();
	lap("finish", "%s", "");
#undef AT
}

// ---- the ELF header, program headers, the Go build id note and a minimal section table
#define PT_LOAD 1
#define PT_NOTE 4
#define PT_GNU_STACK 0x6474e551
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SHT_PROGBITS 1
#define SHT_STRTAB 3
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4

static void writeELF(void) {
	typedef struct {
		u32 typ, flags;
		u64 off, vaddr, filesz, memsz, algn;
	} seg;
	VEC(u8) note = {0};
	size_t idlen = strlen(flagBuildid);
	if (idlen != 0) {
		u8 w[12];
		st32(w, 4);
		st32(w + 4, (u32)idlen);
		st32(w + 8, 4); // ELF_NOTE_GOBUILDID_TAG
		for (int i = 0; i < 12; i++) VPUSH(note, w[i]);
		const char *goName = "Go\0\0";
		for (int i = 0; i < 4; i++) VPUSH(note, (u8)goName[i]);
		for (size_t i = 0; i < idlen; i++) VPUSH(note, (u8)flagBuildid[i]);
		while (note.n % 4 != 0) VPUSH(note, 0);
	}
	seg segs[5] = {
		{PT_LOAD, PF_R | PF_X, 0, imageBase, lay.etext - imageBase, lay.etext - imageBase, 4096},
		{PT_LOAD, PF_R, lay.rodata - imageBase, lay.rodata, lay.etypes - lay.rodata, lay.etypes - lay.rodata, 4096},
		{PT_LOAD, PF_R | PF_W, lay.noptrdata - imageBase, lay.noptrdata, lay.edata - lay.noptrdata, lay.end - lay.noptrdata, 4096},
		{PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 8},
	};
	int nsegs = 4;
	u64 noteOff = 64 + 56 * (u64)(nsegs + 1);
	if (note.n) {
		segs[nsegs++] = (seg){PT_NOTE, PF_R, noteOff, imageBase + noteOff, note.n, note.n, 4};
		if (noteOff + note.n > headerSize) fatalf("build id too long");
		memcpy(out + noteOff, note.a, note.n);
	}
	u32 entry = lookupC(flagEntry, 0);
	if (entry == 0 || addr[entry] == 0) fatalf("missing entry symbol %s", flagEntry);

	// sections, for tools (the runtime needs none of this)
	typedef struct {
		const char *name;
		u32 typ;
		u64 flags;
		u32 addr, size;
		u64 align;
	} sect;
	u32 bi = buildinfoSym;
	sect sects[13] = {
		{"", 0, 0, 0, 0, 0},
		{".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, lay.text, lay.etext - lay.text, 32},
		{".rodata", SHT_PROGBITS, SHF_ALLOC, lay.rodata, lay.erodata - lay.rodata, 32},
		{".gopclntab", SHT_PROGBITS, SHF_ALLOC, lay.pclntab, lay.epclntab - lay.pclntab, 32},
		{".go.type", SHT_PROGBITS, SHF_ALLOC, lay.types, lay.etypes - lay.types, 32},
		{".go.buildinfo", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, addr[bi], rnd(lay.noptrdata + 1, 16) - lay.noptrdata, 16},
		{".noptrdata", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, lay.noptrdata, lay.enoptrdata - lay.noptrdata, 32},
		{".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, lay.data, lay.edata - lay.data, 64},
		{".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, lay.bss, lay.ebss - lay.bss, 64},
		{".noptrbss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, lay.noptrbss, lay.enoptrbss - lay.noptrbss, 32},
	};
	int nsects = 10;
	for (size_t i = 0; i < dataSpecials.n; i++) {
		if (dataSpecials.a[i].gid == bi) {
			sects[5].size = dataSpecials.a[i].size;
			// .noptrdata starts after it, so the two do not overlap
			sects[6].addr = addr[bi] + dataSpecials.a[i].size;
			sects[6].size = lay.enoptrdata - sects[6].addr;
		}
	}
	if (note.n) sects[nsects++] = (sect){".note.go.buildid", SHT_NOTE, SHF_ALLOC, imageBase + (u32)noteOff, (u32)note.n, 4};
	sects[nsects++] = (sect){".shstrtab", SHT_STRTAB, 0, 0, 0, 1};
	VEC(u8) strtab = {0};
	VPUSH(strtab, 0);
	u64 shoff = rnd(lay.edata - imageBase, 8);
	u64 stroff = shoff + 64 * (u64)nsects;
	if (stroff + 256 > outMapped) fatalf("section table does not fit");
	u8 *sh = out + shoff;
	memset(sh, 0, 64 * (size_t)nsects);
	for (int i = 1; i < nsects; i++) {
		sect *s = &sects[i];
		u8 *e = sh + 64 * i;
		st32(e + 0, (u32)strtab.n);
		for (const char *c = s->name; *c; c++) VPUSH(strtab, (u8)*c);
		VPUSH(strtab, 0);
		st32(e + 4, s->typ);
		st64(e + 8, s->flags);
		st64(e + 16, s->addr);
		if (s->addr != 0) st64(e + 24, s->addr - imageBase);
		st64(e + 32, s->size);
		st64(e + 48, s->align);
		if (s->typ == SHT_STRTAB) {
			st64(e + 24, stroff);
			st64(e + 32, strtab.n);
		}
	}
	memcpy(out + stroff, strtab.a, strtab.n);
	outSize = (long long)(stroff + strtab.n);
	if ((size_t)outSize > outMapped) fatalf("section table does not fit");

	u8 *hd = out;
	memcpy(hd, "\x7f" "ELF\x02\x01\x01", 7); // 64-bit, little-endian, version 1; OS ABI 0
	st16(hd + 16, 2);                          // ET_EXEC
	st16(hd + 18, 62);                         // EM_X86_64
	st32(hd + 20, 1);
	st64(hd + 24, addr[entry]);
	st64(hd + 32, 64); // program headers
	st64(hd + 40, shoff);
	st16(hd + 52, 64); // header size
	st16(hd + 54, 56);
	st16(hd + 56, (u16)nsegs);
	st16(hd + 58, 64);
	st16(hd + 60, (u16)nsects);
	st16(hd + 62, (u16)(nsects - 1));
	for (int i = 0; i < nsegs; i++) {
		u8 *p = out + 64 + 56 * i;
		st32(p + 0, segs[i].typ);
		st32(p + 4, segs[i].flags);
		st64(p + 8, segs[i].off);
		st64(p + 16, segs[i].vaddr);
		st64(p + 24, segs[i].vaddr);
		st64(p + 32, segs[i].filesz);
		st64(p + 40, segs[i].memsz);
		st64(p + 48, segs[i].algn);
	}
	if (ftruncate(outFd, outSize) < 0) fatalf("truncate %s: %s", flagOut, strerror(errno));
	close(outFd);
}

// -dumpmap: the symbols whose content the linker writes ("special") and the symbols that name places in the
// image ("boundary"; size 0, whether or not an object refers to them); the objects' symbols and their
// relocations were written by the write pass. The format is secdiff's (link/secdiff/fclinkmap.patch, dumpmap.go).
static void dumpMap(void) {
	FILE *f = mapFile_;
	if (!f) return;
	vecspecial *lists[2] = {&roSpecials, &dataSpecials};
	for (int l = 0; l < 2; l++)
		for (size_t i = 0; i < lists[l]->n; i++) {
			special *sp = &lists[l]->a[i];
			str n = symName(sp->gid);
			fprintf(f, "S %x %u special %.*s\n", addr[sp->gid], sp->size, (int)n.n, n.p);
		}
	boundary def[nBoundaries];
	boundaries(def);
	for (size_t i = 0; i < nBoundaries; i++) fprintf(f, "S %x 0 boundary %s\n", def[i].a, def[i].name);
	fclose(f);
	mapFile_ = NULL;
}
