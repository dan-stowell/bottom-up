// write.c: the output. One parallel pass over the units copies every live symbol into the mapped output file,
// applies its relocations, writes its function-table entry (pclntab) and records the pointer words of data and
// bss symbols for the GC bitmaps.

static u8 *out; // the output file, mapped: out[address-imageBase]
static size_t outMapped;
static int outFd = -1;
static const long long shdrRoom = 4096; // section headers and their names, after the image
static u8 *gcDataBit, *gcBssBit;         // one bit per word of [data, edata): is it a pointer?
static u32 deferreturnAddr, unreachableMethodAddr, mapinitnoopAddr;
static FILE *mapFile_; // -dumpmap
static void writeUnitTask(void *ctx, int w, long ui);

// hugeFree: the huge pages free for a new reservation, from /proc/meminfo (for the error message).
static long hugeFree(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return -1;
	char line[256];
	long fr = -1, rsvd = 0;
	while (fgets(line, sizeof line, f)) {
		sscanf(line, "HugePages_Free: %ld", &fr);
		sscanf(line, "HugePages_Rsvd: %ld", &rsvd);
	}
	fclose(f);
	return fr < 0 ? -1 : fr - rsvd;
}

// createHuge makes the output on hugetlbfs: a new file (write() does not work there; ftruncate + a shared
// mapping does). The shared mapping is made without MAP_NORESERVE, so the kernel reserves every huge page of it
// now or fails the mmap with ENOMEM: a page the write pass touches later is always there (touching an unreserved
// one would be SIGBUS). The pages come zeroed, which is what bss and noptrbss must read as.
static void createHuge(void) {
	outFd = open(outTmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0); // (mode 0 until it is complete)
	if (outFd < 0) fatalf("create %s: %s", outTmp, strerror(errno));
	if (ftruncate(outFd, outSize) < 0) fatalf("truncate %s: %s", outTmp, strerror(errno));
	out = mmap(NULL, (size_t)outSize, PROT_READ | PROT_WRITE, MAP_SHARED, outFd, 0);
	if (out == MAP_FAILED) {
		int e = errno;
		long need = (long)(outSize / hugePage), fr = hugeFree();
		if (e == ENOMEM)
			fatalf("%s is on hugetlbfs and needs %ld free 2 MiB huge pages; %ld are free (raise sysctl vm.nr_hugepages, or "
			       "remove files from the mount: they hold their pages)", flagOut, need, fr);
		fatalf("mmap %s: %s", flagOut, strerror(e));
	}
}

// ---- the output path: claimed at the start, written under a temporary name, renamed into place when complete
// (mold's protocol). claimOutput, the first thing a link does (main, before the inputs are loaded), renames
// whatever is at the output path to a name of its own in the same directory, .<name>.hlink-<pid>-<random>.
// rename is atomic, so of several links to one path exactly one gets the old file; the others find nothing there
// and write new files. From then on the path does not exist until commitOutput renames the complete image there:
// the link's last step, after the file has its final size and mode and is no longer mapped (-teardown exit: it
// is still mapped until the process exits, so an exec of the path in that moment can fail with ETXTBSY; the
// caller's wait returns after it is unmapped).
//
// Reusing the old file's pages (the default on an ordinary file system; never with -fresh, nor on hugetlbfs, where
// a new file's huge pages must be reserved). Unlinking a 110 MB output and filling a new one costs a page-cache
// allocation and a fault per 4 KiB page (31-39 ms on ext4 for MinIO); the pages of the file a rebuild replaces are
// already there (13 ms to fill). So the claimed file is rewritten in place when it is a regular file (the claim
// renames a symlink itself, not its target), ours, with no other hard link, and not being executed (opening it for
// writing fails with ETXTBSY; while we hold it open for writing, nobody can exec it). The claim opens it and sets
// its mode to 0; its hard-link count is checked again on the open file right before the first write (reuseOutput).
// A claimed file that is not reused is unlinked at once, held by an O_PATH descriptor so that freeing its pages
// (15-25 ms for MinIO) happens when the process ends: with -teardown fork, after the caller's wait has returned
// (on hugetlbfs it is not held: the new file needs its huge pages).
//
// What others see:
//   - The output path: absent from the claim until the link is complete, then exactly the new image (the bytes a
//     new file would get: a reused file is zeroed before the write pass), at its final size, with mode
//     0755 & ~umask. Once a link has started, the path never again shows the old executable or a partial image,
//     whether the link succeeds, fails or is killed. Of concurrent links to one path the last to finish wins;
//     each wrote a file of its own.
//   - A failed link, or one ended by SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGBUS or SIGSEGV, removes its
//     temporary (with -teardown fork the parent passes SIGINT, SIGTERM, SIGHUP and SIGQUIT on to the linking child
//     and exits as it does). A link killed by SIGKILL leaves its temporary behind (mode 0 if it is the reused old
//     file): the next link that finds nothing at the output path removes the temporaries of dead links (kill(pid,
//     0) says ESRCH) to the same name that are ours, after its own output is complete (with -teardown fork, after
//     the caller's wait). A leftover survives while every later link finds an output there, which is the case
//     only if another link completed after the killed one had claimed. (A pid seen from another pid namespace
//     sharing the directory looks dead: do not link to one path from two pid namespaces at once.)
//   - What page reuse leaves exposed, the old inode being rewritten in place (-fresh leaves none of it: the old
//     inode is never written):
//       * a process that opened or mapped the old output before the claim keeps the old inode and sees it change:
//         zeros, then the new image, and a new size (its mapping beyond the new size gets SIGBUS). A running
//         executable is never reused, so no running program sees this, but a reader that has the file open does;
//       * a hard link to the temporary made after the last count check, just before the first write. The file
//         has mode 0 by then, so with fs.protected_hardlinks=1 (the default) only its owner or root can make one;
//         with 0, anyone who can write a directory on that file system;
//       * a process that opens the temporary by name between the rename and the chmod to 0 (a few microseconds;
//         it must find the dot name by reading the directory).
static mode_t outMode;       // the mode the file gets when it is complete
static bool outReused;       // the old file's pages are reused
static long long oldOutSize; // (for the -stats note)
static int claimFd = -1;     // the claimed old output, open for writing, mode 0: to be reused
static mode_t claimMode;     // its mode before the claim (restored if it turns out to have another link)
static int heldFd = -1;      // a claimed old output that is not reused: unlinked, freed when the process ends
static bool claimFoundNothing; // nothing was at the output path: sweep dead links' temporaries at the end

// outDirBase splits flagOut into its directory (with the slash; "" for none) and its last element.
static void outDirBase(char *dir, size_t dn, const char **base) {
	const char *slash = strrchr(flagOut, '/');
	size_t n = slash ? (size_t)(slash - flagOut) + 1 : 0;
	if (n >= dn) n = 0;
	memcpy(dir, flagOut, n);
	dir[n] = 0;
	*base = flagOut + n;
}

// letGo closes fd (if any) and unlinks the claimed file at outTmp, which an O_PATH descriptor keeps until exit.
// (Not fd itself: a descriptor open for writing would make an exec of another hard link fail with ETXTBSY.)
static void letGo(int fd) {
	if (fd >= 0) close(fd);
	if (heldFd < 0 && !outOnHugetlbfs) heldFd = open(outTmp, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	unlink(outTmp);
}

static void claimOutput(void) {
	char dir[4096];
	const char *base;
	outDirBase(dir, sizeof dir, &base);
	u32 r;
	if (getrandom(&r, sizeof r, GRND_NONBLOCK) != sizeof r) r = (u32)(nowSeconds() * 1e9);
	int n = snprintf(outTmp, sizeof outTmp, "%s.%s.hlink-%d-%08x", dir, base, (int)getpid(), r);
	if (n < 0 || n >= (int)sizeof outTmp) fatalf("output path too long: %s", flagOut);
	outTmpLive = 1; // (from before the rename: a signal between the two removes nothing that is not ours)
	if (rename(flagOut, outTmp) < 0) {
		// (ENOENT: nothing there; anything else, e.g. EACCES, and creating the temporary fails with the reason)
		claimFoundNothing = errno == ENOENT;
		return;
	}
	struct stat st;
	int fd = -1;
	if (!flagFresh && !outOnHugetlbfs) {
		fd = open(outTmp, O_RDWR | O_NOFOLLOW | O_CLOEXEC); // (fails: ETXTBSY being executed, ELOOP a symlink, EISDIR, ...)
		if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1 &&
		    fchmod(fd, 0) == 0) {
			claimFd = fd;
			claimMode = st.st_mode & 07777;
			oldOutSize = st.st_size;
			return;
		}
	}
	if (lstat(outTmp, &st) == 0 && S_ISDIR(st.st_mode)) { // (a directory is not ours to replace: put it back)
		if (fd >= 0) close(fd);
		rename(outTmp, flagOut);
		outTmpLive = 0;
		fatalf("%s is a directory", flagOut);
	}
	letGo(fd);
}

static void zeroTask(void *ctx UNUSED, int w UNUSED, long i) {
	size_t lo = (size_t)i << 21, hi = MIN(lo + (2u << 20), (size_t)outSize);
	// (populating the chunk's page tables in one call is a little cheaper than a fault per page; the fault is
	// the cost here, about 2 us per 4 KiB page on ext4, the memset is not; an old kernel says EINVAL: no matter)
	madvise(out + lo, hi - lo, 23 /* MADV_POPULATE_WRITE */);
	memset(out + lo, 0, hi - lo);
}

// reuseOutput rewrites the claimed file in place (see above), or lets it go and says false.
static bool reuseOutput(void) {
	int fd = claimFd;
	claimFd = -1;
	struct stat st;
	// the last check before the first write: a hard link made since the claim (then the file is not ours alone:
	// give it its mode back and let go of our name for it)
	if (fstat(fd, &st) < 0 || st.st_nlink != 1) {
		fchmod(fd, claimMode);
		letGo(fd);
		return false;
	}
	if (ftruncate(fd, outSize) < 0) goto fail;
	out = mmap(NULL, (size_t)outSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (out == MAP_FAILED) goto fail;
	outFd = fd;
	outReused = true;
	par((long)(((size_t)outSize + (2u << 20) - 1) >> 21), zeroTask, NULL);
	return true;
fail:
	letGo(fd);
	return false;
}

static void openOutput(void) {
	mode_t um = umask(0);
	umask(um);
	outMode = 0755 & ~um;
	if (outOnHugetlbfs) {
		createHuge();
		return;
	}
	if (claimFd >= 0 && reuseOutput()) return;
	outFd = open(outTmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0); // (mode 0 until it is complete)
	if (outFd < 0) fatalf("create %s: %s", outTmp, strerror(errno));
	if (ftruncate(outFd, outSize) < 0) fatalf("truncate %s: %s", outTmp, strerror(errno));
	out = mmap(NULL, (size_t)outSize, PROT_READ | PROT_WRITE, MAP_SHARED, outFd, 0);
	if (out == MAP_FAILED) fatalf("mmap %s: %s", outTmp, strerror(errno));
}

// closeOutput gives the file its final size and mode and closes it (writeELF, at the end of finish).
static void closeOutput(void) {
	if (ftruncate(outFd, outSize) < 0) fatalf("truncate %s: %s", outTmp, strerror(errno));
	if (fchmod(outFd, outMode) < 0) fatalf("chmod %s: %s", outTmp, strerror(errno));
	close(outFd);
	outFd = -1;
}

// commitOutput renames the complete output into place (main: after closeOutput and the unmap).
static void commitOutput(void) {
	if (rename(outTmp, flagOut) < 0) fatalf("rename %s to %s: %s", outTmp, flagOut, strerror(errno));
	outTmpLive = 0;
}

// sweepStale removes temporaries that dead links to the same output name left (see above). Run after the output
// is complete, and only when the claim found nothing at the path (what a killed link leaves): one directory read.
static void sweepStale(void) {
	if (!claimFoundNothing) return;
	char dir[4096], prefix[4096];
	const char *base;
	outDirBase(dir, sizeof dir, &base);
	int pn = snprintf(prefix, sizeof prefix, ".%s.hlink-", base);
	if (pn < 0 || pn >= (int)sizeof prefix) return;
	DIR *d = opendir(*dir ? dir : ".");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, prefix, (size_t)pn) != 0) continue;
		char *end;
		long pid = strtol(e->d_name + pn, &end, 10);
		if (end == e->d_name + pn || *end != '-' || pid <= 0 || pid == getpid()) continue;
		if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;
		struct stat st;
		if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_uid == geteuid()) unlinkat(dirfd(d), e->d_name, 0);
	}
	closedir(d);
}

// ---- the end (main.c's -teardown)
// unmapOutput empties the output mapping from all threads (MADV_DONTNEED takes the mmap lock shared; the pages
// stay in the file) and then unmaps the empty range. A detached child must do this before it says the output is
// complete: a shared writable mapping holds the file open for writing, and exec of it fails with ETXTBSY until
// the last such mapping is gone.
static void dontneedTask(void *ctx UNUSED, int w UNUSED, long i) {
	size_t lo = (size_t)i << 21, hi = MIN(lo + (2u << 20), outMapped);
	madvise(out + lo, hi - lo, MADV_DONTNEED);
}
static void unmapOutput(void) {
	if (!out || out == MAP_FAILED) return;
	// (MinIO: 4.2 ms on ext4 and 1.0 on tmpfs, against 5.4 and 4.5 for the munmap alone)
	par((long)((outMapped + (2u << 20) - 1) >> 21), dontneedTask, NULL);
	munmap(out, outMapped);
	out = NULL;
}

// releaseInputs empties the inputs' mappings from all threads (-teardown madvise). They are found in
// /proc/self/maps rather than in load.c's tables: every private read-only file mapping that is not a shared
// library or hlink itself.
typedef struct {
	u8 *p;
	size_t n;
} span;
static VEC(span) inputSpans;
static void releaseTask(void *ctx UNUSED, int w UNUSED, long i) { madvise(inputSpans.a[i].p, inputSpans.a[i].n, MADV_DONTNEED); }
static void releaseInputs(void) {
	char exe[4096];
	ssize_t en = readlink("/proc/self/exe", exe, sizeof exe - 1);
	exe[en > 0 ? en : 0] = 0;
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return;
	char line[4400];
	while (fgets(line, sizeof line, f)) {
		unsigned long lo, hi, ino;
		char perm[8];
		int pathAt = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %lu %n", &lo, &hi, perm, &ino, &pathAt) < 4 || ino == 0 || strcmp(perm, "r--p") != 0) continue;
		char *path = line + pathAt;
		path[strcspn(path, "\n")] = 0;
		if (strcmp(path, exe) == 0 || strstr(path, ".so")) continue;
		span s = {(u8 *)lo, hi - lo};
		VPUSH(inputSpans, s);
	}
	fclose(f);
	par((long)inputSpans.n, releaseTask, NULL);
}

static void writeOut(void) {
	long long fileEnd = (long long)(lay.edata - imageBase);
	// (an image for hugetlbfs has bss in the file, and the section table, if any, in a segment's slack)
	outSize = outHuge ? (long long)rnd(lay.end - imageBase, hugePage) : fileEnd + shdrRoom;
	openOutput();
	outMapped = (size_t)outSize;
	gcDataBit = bigalloc((lay.edata - lay.data) / 64 + 8);
	gcBssBit = bigalloc((lay.ebss - lay.bss) / 64 + 8);
	u32 g;
	if ((g = lookupC("runtime.deferreturn", 1)) != 0) deferreturnAddr = addr[g];
	if ((g = lookupC("runtime.unreachableMethod", 1)) != 0) unreachableMethodAddr = addr[g];
	if ((g = lookupC("runtime.mapinitnoop", 1)) != 0) mapinitnoopAddr = addr[g];
	if (outOnHugetlbfs) lap("create", "new file on hugetlbfs, %lld huge pages", outSize / hugePage);
	else if (outReused) lap("create", "overwrote the old output in place (it was %.1f MB), zeroed", (double)oldOutSize / 1e6);
	else lap("create", "new file");
	if (*flagDumpMap) {
		mapFile_ = fopen(flagDumpMap, "w");
		if (!mapFile_) fatalf("create %s: %s", flagDumpMap, strerror(errno));
		for (long ui = 0; ui < nunits; ui++) writeUnitTask(NULL, 0, ui); // serial, in address order of units
	} else {
		par(nunits, writeUnitTask, NULL);
	}
	u64 n = 0;
	for (long ui = 0; ui < nunits; ui++) n += units[ui].nrel;
	lap("write", "%llu relocations applied", (unsigned long long)n);
}

static u32 sectBase(u32 a) {
	if (a < lay.rodata) return lay.text;
	if (a < lay.pclntab) return lay.rodata;
	if (a < lay.types) return lay.pclntab;
	if (a < lay.noptrdata) return lay.types;
	if (a < lay.data) return lay.noptrdata;
	if (a < lay.bss) return lay.data;
	if (a < lay.noptrbss) return lay.bss;
	return lay.noptrbss;
}

static bytes funcInfoOf(u32 c) {
	if (!isDef(c)) return (bytes){0, 0};
	u32 j, na;
	object *o = objOf(c, &j);
	auxp as = oAuxs(o, j, &na);
	for (u32 i = 0; i < na; i++)
		if (aType(AUX(as, i)) == AuxFuncInfo) return dataOf(canon[AREF(o, AUX(as, i))]);
	return (bytes){0, 0};
}

static void undefinedRef(const object *o, const u8 *s, u32 tg) {
	u32 c = canon[tg];
	const char *what = "undefined";
	if (isDef(c)) what = "not reachable (hlink's dead-code pass and the code disagree)";
	str sn = symNameOf(o, s), tn = symName(c);
	errorf("%.*s: relocation target %.*s %s", (int)sn.n, sn.p, (int)tn.n, tn.p, what);
}

// typePtrs appends to v the index of every pointer word of a value of type t.
typedef VEC(i64) veci64;
static __thread veci64 ptrScratch;
static void typePtrs(u32 t, veci64 *v) {
	u32 j, nr;
	object *o = objOf(t, &j);
	bytes p = oData(o, j);
	if (p.n < 48) return;
	i64 ptrdata = (i64)ld64(p.p + 8);
	if (ptrdata == 0) return;
	relocp rs = oRelocs(o, j, &nr);
#define TARGET(off)                                                                \
	({                                                                             \
		relocp rl_ = relocAt(rs, nr, (off));                                       \
		u32 c_ = 0;                                                                \
		if (rl_) {                                                                 \
			c_ = canon[RREF(o, rl_)];                                              \
			if (!isDef(c_)) c_ = 0;                                                \
		}                                                                          \
		c_;                                                                        \
	})
	switch (typeKind(p.p)) {
	default: {
		if (p.p[ABI_TFlagOff8] & ABI_TFlagGCMaskOnDemand) {
			str n = symNameOf(o, osym(o, j));
			errorf("%.*s: GC mask not available", (int)n.n, n.p);
			return;
		}
		bytes mask = dataOf(TARGET(32));
		for (i64 i = 0; i < ptrdata / 8 && i / 8 < (i64)mask.n; i++)
			if ((mask.p[i / 8] >> (i % 8)) & 1) VPUSH(*v, i);
		break;
	}
	case ABI_KindArray: {
		u32 elem = TARGET(48);
		i64 n = (i64)ld64(p.p + 48 + 16);
		bytes ed = dataOf(elem);
		if (elem == 0 || ed.n < 48) return;
		i64 esize = (i64)ld64(ed.p) / 8;
		size_t start = v->n;
		typePtrs(elem, v);
		size_t ne = v->n - start;
		if (n == 0) {
			v->n = start;
			break;
		}
		for (i64 k = 1; k < n; k++)
			for (size_t q = 0; q < ne; q++) VPUSH(*v, k * esize + v->a[start + q]);
		break;
	}
	case ABI_KindStruct: {
		u32 n = (u32)ld64(p.p + 48 + 16);
		int fo = abiRTypeSize8[ABI_KindStruct];
		if (p.p[ABI_TFlagOff8] & ABI_TFlagUncommon) fo += ABI_UncommonSize;
		for (u32 i = 0; i < n; i++) {
			int f = fo + (int)i * ABI_StructFieldSize8;
			u32 ft = TARGET(f + 8);
			if (ft == 0) continue;
			i64 foff = (i64)ld64(p.p + f + 16) / 8;
			size_t start = v->n;
			typePtrs(ft, v);
			for (size_t q = start; q < v->n; q++) v->a[q] += foff;
		}
		break;
	}
	}
#undef TARGET
}

// gcSym sets, in bitmap bm, the bits of the pointer words of data symbol j of o, which is at byte offset off of the
// bitmap's section. The symbol's Go type says where its pointers are (ld.GCProg.AddSym).
static void gcSym(u8 *bm, const object *o, u32 j, u32 off) {
	u32 na;
	auxp as = oAuxs(o, j, &na);
	for (u32 i = 0; i < na; i++) {
		if (aType(AUX(as, i)) == AuxGotype) {
			u32 t = canon[AREF(o, AUX(as, i))];
			if (isDef(t)) {
				veci64 *v = &ptrScratch;
				v->n = 0;
				typePtrs(t, v);
				for (size_t q = 0; q < v->n; q++) {
					i64 w = v->a[q] + off / 8;
					bm[w >> 3] |= (u8)(1u << (w & 7));
				}
			}
			return;
		}
	}
}

static void dumpSym(u32 a, u32 size, const char *bucket, str name) {
	fprintf(mapFile_, "S %x %u %s %.*s\n", a, size, bucket, (int)name.n, name.p);
}

static void writeUnitTask(void *ctx UNUSED, int w UNUSED, long ui) {
	unit *u = &units[ui];
	object *o = u->o;
	// the function table is in address order: within the unit's block, by alignment class from the largest down
	// (fclink numbers the functions in symbol order, which is only right while they all have one class)
	u32 fidx[2][nClasses];
	for (int t = 0; t < 2; t++) {
		u32 k = u->funcBase[t];
		for (int cl = nClasses - 1; cl >= 0; cl--) {
			fidx[t][cl] = k;
			k += u->nfuncCl[t][cl];
		}
	}
	u32 foff = lay.ftabStructs + u->ftabBase;
	u32 inl = u->inlBase;
	u8 *names = out + (lay.funcnametab - imageBase);
	u8 *pctab = out + (lay.pctab - imageBase);
	u64 nrel = 0;
	for (u32 j = u->lo; j < MIN(u->hi, o->nalldef); j++) {
		u32 gid = o->base + j;
		u32 st = state[gid];
		if ((st & (stLive | stPC | stNameNeeded)) == 0 || canon[gid] != gid) continue;
		const u8 *s = osym(o, j);
		if (st & stPC) {
			u32 a = addr[gid];
			if (a != 0) {
				bytes d = oData(o, j);
				memcpy(pctab + a, d.p, d.n);
				if (mapFile_) dumpSym(lay.pctab + a, d.n, "pctab", symNameOf(o, s));
			}
		}
		int b = (int)(st >> stBucketShift & 31);
		bool text = isTextBucket(b);
		if (text || (st & stNameNeeded)) {
			str n = symNameOf(o, s);
			memcpy(names + aux[gid], n.p, n.n);
		}
		if (b == bNone) continue;
		u32 a = addr[gid];
		u32 siz = symSiz(s);
		if (mapFile_) dumpSym(a, siz, bucketName[b], symNameOf(o, s));
		switch (b) {
		case bBSS: gcSym(gcBssBit, o, j, a - lay.bss); continue;
		case bNoptrBSS:
		case bCov: continue;
		case bData:
		case bDataFIPS: gcSym(gcDataBit, o, j, a - lay.data); break;
		}
		bytes d = oData(o, j);
		if (d.n > siz) {
			str n = symNameOf(o, s);
			errorf("%.*s: initialize bounds (%u < %u)", (int)n.n, n.p, siz, d.n);
			continue;
		}
		u8 *P = out + (a - imageBase);
		memcpy(P, d.p, d.n);
		if (text) memset(P + siz, 0xCC, occupied(siz, b, st >> stClassShft & 15) - siz); // INT3
		u32 nr;
		relocp rs = oRelocs(o, j, &nr);
		nrel += nr;
		u32 deferreturn = 0;
		for (u32 i = 0; i < nr; i++) {
			relocp rl = RELOC(rs, i);
			u32 rsiz = rSiz(rl);
			if (rsiz == 0) continue; // a marker for the dead-code pass
			u16 t = rType(rl);
			bool weak = (t & R_WEAK) != 0;
			t &= (u16)~R_WEAK;
			u32 off = (u32)rOff(rl);
			if ((u64)off + rsiz > siz) {
				str n = symNameOf(o, s);
				errorf("%.*s: invalid relocation at %d+%u, size %u", (int)n.n, n.p, (int)off, rsiz, siz);
				continue;
			}
			u32 tg = RREF(o, rl);
			u32 ta = addr[tg];
			i64 v;
			switch (t) {
			case R_ADDR:
				if (ta == 0 && tg != 0) {
					if (!weak) {
						undefinedRef(o, s, tg);
						continue;
					}
					ta = unreachableMethodAddr; // a method nothing can call: throws if called
				}
				v = (i64)ta + rAdd(rl);
				if (rsiz == 4 && (i32)v < 0) {
					str n = symNameOf(o, s), tn = symName(canon[tg]);
					errorf("%.*s: non-pc-relative relocation address for %.*s is too big: %#llx", (int)n.n, n.p, (int)tn.n, tn.p, (unsigned long long)v);
				}
				break;
			case R_METHODOFF:
			case R_ADDROFF:
				if (ta == 0) {
					if (t == R_METHODOFF) {
						st32(P + off, 0xFFFFFFFFu); // the runtime knows -1 is no method
						if (mapFile_) {
							str tn = symName(canon[tg]);
							fprintf(mapFile_, "R %x %u %u %x %lld %.*s\n", a + off, rsiz, t, 0u, (long long)rAdd(rl), (int)tn.n, tn.p);
						}
						continue;
					}
					if (!weak) undefinedRef(o, s, tg);
					continue;
				}
				v = (i64)(ta - sectBase(ta)) + rAdd(rl);
				break;
			case R_CALL:
			case R_PCREL:
				if (ta == 0 && tg != 0) {
					if (!(weak && t == R_CALL)) {
						undefinedRef(o, s, tg);
						continue;
					}
					ta = mapinitnoopAddr; // the initializer of a map variable that is not live
				}
				if (t == R_CALL && ta == deferreturnAddr && deferreturn == 0 && text) deferreturn = off - 1; // the call's start
				v = (i64)ta + rAdd(rl) - (i64)(a + off + rsiz);
				if (tg == 0) v = rAdd(rl) - (i64)(a + off + rsiz);
				if (v != (i64)(i32)v) {
					str n = symNameOf(o, s), tn = symName(canon[tg]);
					errorf("%.*s: pc-relative relocation to %.*s is too far", (int)n.n, n.p, (int)tn.n, tn.p);
				}
				break;
			case R_TLS_LE: v = -8 + rAdd(rl); break; // ELF: g is at -8(FS)
			case R_CONST: v = rAdd(rl); break;
			case R_SIZE: {
				u32 c = canon[tg];
				v = 0;
				if (isDef(c)) {
					u32 j2;
					object *o2 = objOf(c, &j2);
					v = symSiz(osym(o2, j2));
				}
				v += rAdd(rl);
				break;
			}
			default: {
				str n = symNameOf(o, s), tn = symName(canon[tg]);
				char buf[32];
				const char *tname = t < NRELOCTYPE ? relocTypeName[t] : (snprintf(buf, sizeof buf, "RelocType(%u)", t), buf);
				errorf("%.*s: unsupported relocation type %s to %.*s", (int)n.n, n.p, tname, (int)tn.n, tn.p);
				continue;
			}
			}
			switch (rsiz) {
			case 8: st64(P + off, (u64)v); break;
			case 4: st32(P + off, (u32)v); break;
			case 2: st16(P + off, (u16)v); break;
			case 1: P[off] = (u8)v; break;
			default: {
				str n = symNameOf(o, s);
				errorf("%.*s: bad relocation size %u", (int)n.n, n.p, rsiz);
			}
			}
			if (mapFile_) {
				str tn = symName(canon[tg]);
				fprintf(mapFile_, "R %x %u %u %x %lld %.*s\n", a + off, rsiz, t, ta, (long long)rAdd(rl), (int)tn.n, tn.p);
			}
		}
		if (!text) continue;
		// ---- the function's entry in the pc -> function table, and its _func
		u32 npc, nfd, ninl;
		bytes fi = funcShape(o, j, &npc, &nfd, &ninl);
		u8 *ft = out + (lay.functab - imageBase);
		u32 fi0 = fidx[b - bText][st >> stClassShft & 15]++;
		st32(ft + fi0 * 8, a - lay.text);
		st32(ft + fi0 * 8 + 4, foff);
		u8 *f = ft + foff;
		st32(f + 0, a - lay.text);
		st32(f + 4, aux[gid]);
		st32(f + 12, deferreturn);
		st32(f + 28, npc);
		st32(f + 32, o->cuOff);
		f[43] = (u8)nfd;
		if (fi.n != 0) {
			memcpy(f + 8, fi.p + 0, 4);   // args
			memcpy(f + 36, fi.p + 12, 4); // startLine
			f[40] = fi.p[8];              // funcID
			f[41] = fi.p[9];              // flag
			u8 *pcd = f + funcSize, *fdd = f + funcSize + 4 * npc;
			for (u32 k = 0; k < nfd; k++) st32(fdd + 4 * k, 0xFFFFFFFFu);
			u32 na;
			auxp as = oAuxs(o, j, &na);
			u32 kp = 0, kf = 0, pcinline = 0;
			for (u32 i = 0; i < na; i++) {
				auxp x = AUX(as, i);
				switch (aType(x)) {
				case AuxPcsp: st32(f + 16, addr[AREF(o, x)]); break;
				case AuxPcfile: st32(f + 20, addr[AREF(o, x)]); break;
				case AuxPcline: st32(f + 24, addr[AREF(o, x)]); break;
				case AuxPcinline: pcinline = addr[AREF(o, x)]; break;
				case AuxPcdata: st32(pcd + 4 * kp, addr[AREF(o, x)]); kp++; break;
				case AuxFuncdata: {
					// (a symbol nothing defines is an assembly function's missing args_stackmap: no value)
					u32 ta = addr[AREF(o, x)];
					if (ta != 0) st32(fdd + 4 * kf, ta - lay.gofunc);
					kf++;
					break;
				}
				}
			}
			if (ninl > 0) {
				st32(pcd + 4 * ABI_PCDATA_InlTreeIndex, pcinline);
				u32 inlOff;
				fiNumInl(fi, &inlOff);
				u8 *tt = out + (inl - imageBase);
				for (u32 k = 0; k < ninl; k++) {
					const u8 *node = fiInlNode(fi, inlOff, k);
					u32 callee = canon[oref(o, ld32(node + 12), ld32(node + 16))];
					u8 *e = tt + 16 * k;
					bytes cfi = funcInfoOf(callee);
					if (cfi.n != 0) {
						e[0] = cfi.p[8];              // funcID
						memcpy(e + 12, cfi.p + 12, 4); // startLine
					}
					st32(e + 4, aux[callee]);
					st32(e + 8, ld32(node + 20)); // ParentPC
				}
				st32(fdd + 4 * ABI_FUNCDATA_InlTree, inl - lay.gofunc);
				if (mapFile_) {
					str n = symNameOf(o, s);
					fprintf(mapFile_, "S %x %u inltree %.*s.inltree\n", inl, 16 * ninl, (int)n.n, n.p);
				}
				inl += 16 * ninl;
			}
		}
		foff += rnd(funcSize + 4 * npc + 4 * nfd, 8);
	}
	u->nrel = nrel;
}
