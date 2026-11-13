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

static void writeOut(void) {
	long long fileEnd = (long long)(lay.edata - imageBase);
	outSize = fileEnd + shdrRoom;
	outFd = open(flagOut, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	if (outFd < 0) fatalf("open %s: %s", flagOut, strerror(errno));
	if (ftruncate(outFd, outSize) < 0) fatalf("truncate %s: %s", flagOut, strerror(errno));
	out = mmap(NULL, (size_t)outSize, PROT_READ | PROT_WRITE, MAP_SHARED, outFd, 0);
	if (out == MAP_FAILED) fatalf("mmap %s: %s", flagOut, strerror(errno));
	outMapped = (size_t)outSize;
	gcDataBit = bigalloc((lay.edata - lay.data) / 64 + 8);
	gcBssBit = bigalloc((lay.ebss - lay.bss) / 64 + 8);
	u32 g;
	if ((g = lookupC("runtime.deferreturn", 1)) != 0) deferreturnAddr = addr[g];
	if ((g = lookupC("runtime.unreachableMethod", 1)) != 0) unreachableMethodAddr = addr[g];
	if ((g = lookupC("runtime.mapinitnoop", 1)) != 0) mapinitnoopAddr = addr[g];
	lap("create", "%s", "");
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
	u32 fidx[2] = {u->funcBase[0], u->funcBase[1]};
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
		u32 fi0 = fidx[b - bText]++;
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
