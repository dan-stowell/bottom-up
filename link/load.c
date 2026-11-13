// load.c: the inputs. Every file of the importcfg is mapped, the Go objects in it are found, and every symbol
// slot of every object gets a dense global id.

static VEC(void *) inputMaps;
static pthread_mutex_t mapMu = PTHREAD_MUTEX_INITIALIZER;
static strmap primary; // package path (escaped) -> index of the object that holds its package-indexed symbols

static bytes mapFile(const char *name) {
	int fd = open(name, O_RDONLY | O_CLOEXEC);
	if (fd < 0) fatalf("open %s: %s", name, strerror(errno));
	struct stat st;
	if (fstat(fd, &st) < 0) fatalf("stat %s: %s", name, strerror(errno));
	if (st.st_size == 0) {
		close(fd);
		return (bytes){0, 0};
	}
	if (st.st_size >= (1ll << 32)) fatalf("%s: too large", name);
	void *b = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (b == MAP_FAILED) fatalf("mmap %s: %s", name, strerror(errno));
	close(fd);
	pthread_mutex_lock(&mapMu);
	VPUSH(inputMaps, b);
	pthread_mutex_unlock(&mapMu);
	return (bytes){b, (u32)st.st_size};
}

static bool bhasPrefix(bytes b, const char *pre, size_t n) { return b.n >= n && memcmp(b.p, pre, n) == 0; }

// goObject returns the object-file part of a "go object" file image (what follows the text header), or nil.
static bytes goObject(bytes body) {
	if (!bhasPrefix(body, "go object ", 10)) return (bytes){0, 0};
	const u8 *k = memmem(body.p, body.n, "\n!\n", 3);
	if (!k) return (bytes){0, 0};
	return (bytes){k + 3, (u32)(body.p + body.n - (k + 3))};
}

static object *newObject(str pkg, const char *file, bytes body) {
	if (body.n >= 4 && memcmp(body.p, "\x7f" "ELF", 4) == 0)
		fatalf("%s: host object in package %.*s: hlink links Go objects only (CGO_ENABLED=0)", file, (int)pkg.n, pkg.p);
	bytes b = goObject(body);
	if (b.p == NULL) return NULL;
	if (b.n < GOOBJ_HEADER_SIZE || memcmp(b.p, GOOBJ_MAGIC, GOOBJ_MAGIC_LEN) != 0) fatalf("%s: not a Go object of this toolchain", file);
	object *o = calloc(1, sizeof *o);
	o->pkg = pkg;
	o->file = file;
	o->b = b.p;
	o->flags = ld32(b.p + GOOBJ_MAGIC_LEN + 8);
	for (int i = 0; i < NBlk; i++) o->off[i] = ld32(b.p + GOOBJ_MAGIC_LEN + 12 + 4 * i);
	for (int i = 0; i < NBlk; i++)
		if (o->off[i] > b.n || (i > 0 && o->off[i] < o->off[i - 1])) fatalf("%s: malformed Go object", file);
	if (o->flags & ObjFlagUnlinkable) fatalf("%s: unlinkable object (package %.*s)", file, (int)pkg.n, pkg.p);
	o->fromAsm = (o->flags & ObjFlagFromAssembly) != 0;
	o->ndef = (o->off[BlkSymdef + 1] - o->off[BlkSymdef]) / SymSize;
	o->nh64 = (o->off[BlkHashed64def + 1] - o->off[BlkHashed64def]) / SymSize;
	o->nh = (o->off[BlkHasheddef + 1] - o->off[BlkHasheddef]) / SymSize;
	o->nnonpkg = (o->off[BlkNonpkgdef + 1] - o->off[BlkNonpkgdef]) / SymSize;
	o->nref = (o->off[BlkNonpkgref + 1] - o->off[BlkNonpkgref]) / SymSize;
	o->npkg = (o->off[BlkPkgIdx + 1] - o->off[BlkPkgIdx]) / 8;
	o->nalldef = o->ndef + o->nh64 + o->nh + o->nnonpkg;
	o->total = o->nalldef + o->nref;
	// Put the pages the flood will read at random in the page table now, in bulk: the index tables, the
	// relocations and the aux records. A prefetch of a page that is not mapped yet does nothing, so without
	// this the flood's cache misses queue up behind page faults instead of overlapping. What is read in order
	// (symbol records, hashes, names: resolve; the data block: the write pass) is left to first touch, which
	// costs less per page with the kernel's fault-around than it does here.
	if (!getenv("HLINK_NOPOPULATE")) {
		uintptr_t lo = ((uintptr_t)b.p + o->off[BlkRelocIdx]) & ~(uintptr_t)4095, hi = ((uintptr_t)b.p + o->off[BlkData] + 4095) & ~(uintptr_t)4095;
		madvise((void *)lo, hi - lo, 22 /* MADV_POPULATE_READ */);
	}
	return o;
}

// filepath.Abs and Clean, lexically (no symbolic links resolved), for comparing the main object's path
static char *absClean(const char *path) {
	char *full;
	if (path[0] == '/') {
		full = strdup(path);
	} else {
		char cwd[4096];
		if (!getcwd(cwd, sizeof cwd)) return strdup(path);
		if (asprintf(&full, "%s/%s", cwd, path) < 0) fatalf("out of memory");
	}
	// Clean
	size_t n = strlen(full);
	char *out = malloc(n + 2);
	size_t w = 0;
	out[w++] = '/';
	size_t r = 1;
	while (r < n) {
		if (full[r] == '/') {
			r++;
		} else if (full[r] == '.' && (r + 1 == n || full[r + 1] == '/')) {
			r++;
		} else if (full[r] == '.' && full[r + 1] == '.' && (r + 2 == n || full[r + 2] == '/')) {
			r += 2;
			if (w > 1) {
				w--;
				while (w > 1 && out[w - 1] != '/') w--;
				if (w > 1) w--;
			}
		} else {
			if (w > 1) out[w++] = '/';
			while (r < n && full[r] != '/') out[w++] = full[r++];
		}
	}
	out[w] = 0;
	free(full);
	return out;
}

// strconv.Unquote of a Go string literal ("..." with escapes, or `...`); NULL if it is not one
static char *goUnquote(const char *s, u32 *outLen) {
	size_t n = strlen(s);
	if (n < 2) return NULL;
	char q = s[0];
	if (q != s[n - 1]) return NULL;
	if (q == '`') {
		if (memchr(s + 1, '`', n - 2)) return NULL;
		char *out = malloc(n), *w = out;
		for (size_t i = 1; i + 1 < n; i++)
			if (s[i] != '\r') *w++ = s[i];
		*w = 0;
		*outLen = (u32)(w - out);
		return out;
	}
	if (q != '"' && q != '\'') return NULL;
	char *out = malloc(4 * n + 4), *w = out;
	for (size_t i = 1; i + 1 < n;) {
		unsigned char c = (unsigned char)s[i];
		if (c == (unsigned char)q && q == '"') { free(out); return NULL; }
		if (c == '\n') { free(out); return NULL; }
		if (c != '\\') {
			*w++ = (char)c;
			i++;
			continue;
		}
		i++;
		if (i + 1 > n - 1) { free(out); return NULL; }
		char e = s[i++];
		u32 v = 0;
		int nd = 0;
		switch (e) {
		case 'a': *w++ = '\a'; continue;
		case 'b': *w++ = '\b'; continue;
		case 'f': *w++ = '\f'; continue;
		case 'n': *w++ = '\n'; continue;
		case 'r': *w++ = '\r'; continue;
		case 't': *w++ = '\t'; continue;
		case 'v': *w++ = '\v'; continue;
		case '\\': *w++ = '\\'; continue;
		case '\'': *w++ = '\''; continue;
		case '"': *w++ = '"'; continue;
		case 'x': nd = 2; break;
		case 'u': nd = 4; break;
		case 'U': nd = 8; break;
		case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
			v = (u32)(e - '0');
			for (int k = 0; k < 2; k++) {
				if (i >= n - 1 || s[i] < '0' || s[i] > '7') { free(out); return NULL; }
				v = v * 8 + (u32)(s[i++] - '0');
			}
			if (v > 255) { free(out); return NULL; }
			*w++ = (char)v;
			continue;
		}
		default: free(out); return NULL;
		}
		for (int k = 0; k < nd; k++) {
			if (i >= n - 1) { free(out); return NULL; }
			char h = s[i++];
			int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
			if (d < 0) { free(out); return NULL; }
			v = v * 16 + (u32)d;
		}
		if (e == 'x') {
			*w++ = (char)v;
			continue;
		}
		if (v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) { free(out); return NULL; }
		// UTF-8
		if (v < 0x80) *w++ = (char)v;
		else if (v < 0x800) { *w++ = (char)(0xC0 | v >> 6); *w++ = (char)(0x80 | (v & 0x3F)); }
		else if (v < 0x10000) { *w++ = (char)(0xE0 | v >> 12); *w++ = (char)(0x80 | ((v >> 6) & 0x3F)); *w++ = (char)(0x80 | (v & 0x3F)); }
		else { *w++ = (char)(0xF0 | v >> 18); *w++ = (char)(0x80 | ((v >> 12) & 0x3F)); *w++ = (char)(0x80 | ((v >> 6) & 0x3F)); *w++ = (char)(0x80 | (v & 0x3F)); }
	}
	*w = 0;
	*outLen = (u32)(w - out);
	return out;
}

typedef struct {
	str pkg;
	const char *file;
	VEC(object *) objs;
} lib;
static lib *libs;
static long nlibs;

static const char *fileExt(const char *name, size_t n) {
	for (size_t i = n; i > 0; i--) {
		if (name[i - 1] == '/') return name + n;
		if (name[i - 1] == '.') return name + i - 1;
	}
	return name + n;
}

static void loadLib(void *ctx UNUSED, int w UNUSED, long i) {
	lib *l = &libs[i];
	bytes b = mapFile(l->file);
	if (!bhasPrefix(b, "!<arch>\n", 8)) {
		object *o = newObject(l->pkg, l->file, b);
		if (o) VPUSH(l->objs, o);
		else errorf("%s: not a Go object or archive", l->file);
		return;
	}
	for (size_t off = 8; off + 60 <= b.n;) {
		const char *h = (const char *)b.p + off;
		// name: strings.TrimSpace(b[off:off+16]); size: strconv.Atoi(strings.TrimSpace(b[off+48:off+58]))
		size_t ns = 0, ne = 16;
		while (ns < ne && (h[ns] == ' ' || h[ns] == '\t' || h[ns] == '\n')) ns++;
		while (ne > ns && (h[ne - 1] == ' ' || h[ne - 1] == '\t' || h[ne - 1] == '\n')) ne--;
		size_t ss = 48, se = 58;
		while (ss < se && h[ss] == ' ') ss++;
		while (se > ss && h[se - 1] == ' ') se--;
		long size = 0;
		bool ok = se > ss;
		for (size_t k = ss; k < se; k++) {
			if (h[k] < '0' || h[k] > '9') { ok = false; break; }
			size = size * 10 + (h[k] - '0');
			if (size > (1l << 40)) { ok = false; break; }
		}
		if (!ok || off + 60 + (size_t)size > b.n) {
			errorf("%s: malformed archive", l->file);
			return;
		}
		bytes body = {b.p + off + 60, (u32)size};
		off += 60 + (size_t)size + (size_t)(size & 1);
		const char *name = h + ns;
		size_t nlen = ne - ns;
		if (nlen > 0 && name[nlen - 1] == '/') nlen--;
		if (nlen == 9 && memcmp(name, "__.PKGDEF", 9) == 0) continue;
		if (nlen < 16) {
			const char *ext = fileExt(name, nlen);
			size_t elen = (size_t)(name + nlen - ext);
			if (!(elen == 2 && memcmp(ext, ".o", 2) == 0) && !(elen == 5 && memcmp(ext, ".syso", 5) == 0)) continue;
		}
		object *o = newObject(l->pkg, l->file, body);
		if (o) VPUSH(l->objs, o);
	}
}

static void loadPkgBase(void *ctx UNUSED, int w UNUSED, long i) {
	object *o = objs[i];
	// (after the packages: the bases of this object's own blocks, in the order of the special package indices
	// from PkgIdxSelf up; see oref)
	if (o->npkg == 0) o->npkg = 1;
	o->pkgBase = malloc((o->npkg + 5) * sizeof(u32));
	o->pkgBase[0] = 0; // the nil reference {0, 0}
	u32 *sb = o->pkgBase + o->npkg;
	sb[PkgIdxSelf - PkgIdxSelf] = o->base;
	sb[PkgIdxBuiltin - PkgIdxSelf] = 0;
	sb[PkgIdxHashed - PkgIdxSelf] = o->base + o->ndef + o->nh64;
	sb[PkgIdxHashed64 - PkgIdxSelf] = o->base + o->ndef;
	sb[PkgIdxNone - PkgIdxSelf] = o->base + o->ndef + o->nh64 + o->nh;
	for (u32 p = 1; p < o->npkg; p++) {
		long *q = smFind(&primary, oPkg(o, p));
		o->pkgBase[p] = q ? objs[*q]->base : badBase;
	}
}

static bool hasSuffixC(const char *s, const char *suf) {
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && strcmp(s + a - b, suf) == 0;
}

static void load(const char *cfgFile, const char *mainFile) {
	int fd = open(cfgFile, O_RDONLY);
	if (fd < 0) fatalf("open %s: %s", cfgFile, strerror(errno));
	struct stat st;
	fstat(fd, &st);
	char *cfg = malloc((size_t)st.st_size + 1);
	ssize_t got = 0;
	while (got < st.st_size) {
		ssize_t k = read(fd, cfg + got, (size_t)(st.st_size - got));
		if (k <= 0) fatalf("read %s: %s", cfgFile, strerror(errno));
		got += k;
	}
	cfg[got] = 0;
	close(fd);
	VEC(lib) ls = {0};
	char *mainAbs = absClean(mainFile);
	bool haveMain = false;
	int lineno = 0;
	for (char *line = cfg, *next; line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		lineno++;
		// strings.TrimSpace
		while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\v' || *line == '\f') line++;
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r' || line[n - 1] == '\v' || line[n - 1] == '\f')) line[--n] = 0;
		if (n == 0 || line[0] == '#') continue;
		char *sp = strchr(line, ' ');
		char *args = sp ? sp + 1 : line + n;
		if (sp) *sp = 0;
		while (*args == ' ' || *args == '\t') args++;
		if (strcmp(line, "packagefile") == 0) {
			char *eq = strchr(args, '=');
			if (!eq || eq == args || eq[1] == 0) fatalf("%s:%d: invalid packagefile", cfgFile, lineno);
			*eq = 0;
			lib l = {cstr(args), eq + 1, {0}};
			char *abs = absClean(eq + 1);
			if (strcmp(abs, mainAbs) == 0) {
				l.pkg = cstr("main");
				haveMain = true;
			}
			free(abs);
			VPUSH(ls, l);
		} else if (strcmp(line, "modinfo") == 0) {
			u32 slen;
			char *s = goUnquote(args, &slen);
			if (!s) fatalf("%s:%d: invalid modinfo: invalid syntax", cfgFile, lineno);
			setStrdata("runtime.modinfo", (str){s, slen});
		} else if (strcmp(line, "packageshlib") == 0) {
			fatalf("%s:%d: shared libraries are not supported", cfgFile, lineno);
		} else {
			fatalf("%s:%d: unknown directive \"%s\"", cfgFile, lineno, line);
		}
	}
	if (!haveMain) {
		lib l = {cstr("main"), mainFile, {0}};
		VPUSH(ls, l);
	}
	libs = ls.a;
	nlibs = (long)ls.n;
	par(nlibs, loadLib, NULL);
	if (nerrs != 0) return;

	u32 next = 1;
	u64 nsym[5] = {0};
	long bareSlots = 0;
	size_t nobj = 0, nun = 0;
	for (long i = 0; i < nlibs; i++) {
		nobj += libs[i].objs.n;
		for (size_t k = 0; k < libs[i].objs.n; k++) nun += libs[i].objs.a[k]->total / unitSlots + 1;
	}
	objs = malloc(MAX(nobj, 1) * sizeof *objs);
	units = calloc(MAX(nun, 1), sizeof *units);
	smInit(&primary, nobj);
	for (long i = 0; i < nlibs; i++) {
		for (size_t k = 0; k < libs[i].objs.n; k++) {
			object *o = libs[i].objs.a[k];
			o->idx = nobjs;
			o->base = next;
			if ((u64)next + o->total >= 0xF0000000u) fatalf("too many symbols");
			next += o->total;
			objs[nobjs++] = o;
			char *pp = pathToPrefix(o->pkg); // (objects name packages by their escaped paths)
			smPut(&primary, cstr(pp), o->idx, NULL);
			if (streq(o->pkg, cstr("main")) && mainObj == NULL) mainObj = o;
			nsym[0] += o->ndef;
			nsym[1] += o->nh64;
			nsym[2] += o->nh;
			nsym[3] += o->nnonpkg;
			nsym[4] += o->nref;
			if (libs[i].objs.n == 1 && !hasSuffixC(o->file, "-d") && hasSuffixC(o->file, ".o")) bareSlots += o->total;
			for (u32 lo = 0; lo < o->total || lo == 0; lo += unitSlots) {
				unit *u = &units[nunits++];
				u->o = o;
				u->lo = lo;
				u->hi = MIN(lo + unitSlots, o->total);
			}
		}
	}
	nslots = next - 1;
	size_t npage = (nslots >> pageShift) + 2;
	objPage = bigalloc(npage * sizeof(u32));
	int k = 0;
	for (size_t p = 0; p < npage; p++) {
		u32 g = (u32)p << pageShift;
		while (k + 1 < nobjs && objs[k]->base + objs[k]->total <= g) k++;
		objPage[p] = (u32)k;
	}
	par(nobjs, loadPkgBase, NULL);
	lap("load",
	    "%ld files, %d objects, %ld units; slots: %llu by index, %llu+%llu by content hash, %llu defined by name, %llu "
	    "references by name; %ld of the slots in bare .o files",
	    nlibs, nobjs, nunits, (unsigned long long)nsym[0], (unsigned long long)nsym[1], (unsigned long long)nsym[2],
	    (unsigned long long)nsym[3], (unsigned long long)nsym[4], bareSlots);
}
