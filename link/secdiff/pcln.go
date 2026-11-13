// pcln.go: the runtime metadata: pclntab, moduledata, the symbols the linker writes, the GC bitmaps of data
// and bss, the order of the types section, the ELF header.
package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"internal/abi"
	"sort"
	"unsafe"
)

// ---- moduledata (runtime/symtab.go)

var mdLayout = []struct {
	name  string
	words int
}{
	{"pcHeader", 1}, {"funcnametab", 3}, {"cutab", 3}, {"filetab", 3}, {"pctab", 3}, {"pclntable", 3}, {"ftab", 3},
	{"findfunctab", 1}, {"minpc", 1}, {"maxpc", 1},
	{"text", 1}, {"etext", 1}, {"noptrdata", 1}, {"enoptrdata", 1}, {"data", 1}, {"edata", 1}, {"bss", 1}, {"ebss", 1},
	{"noptrbss", 1}, {"enoptrbss", 1}, {"covctrs", 1}, {"ecovctrs", 1}, {"end", 1}, {"gcdata", 1}, {"gcbss", 1},
	{"types", 1}, {"typedesclen", 1}, {"etypes", 1}, {"itaboffset", 1}, {"itabsize", 1}, {"rodata", 1}, {"gofunc", 1},
	{"epclntab", 1}, {"textsectmap", 3}, {"ptab", 3}, {"pluginpath", 2}, {"pkghashes", 3}, {"inittasks", 3},
	{"modulename", 2}, {"modulehashes", 3},
	// then hasmain uint8, bad bool, gcdatamask, gcbssmask bitvector, typemap, next: all built at run time but hasmain
}

type side struct {
	name string
	m    *lmap
	img  *image
	md   map[string][]uint64
	mdAt uint64
	mdSz uint64
	tail []byte // moduledata after modulehashes
	// pclntab
	nfunc   int
	funcs   []fn
	byEntry map[uint64]int

	pctabRefd, pctabUnref uint64
}

type fn struct {
	entry, end                                   uint64
	funcAddr                                     uint64
	entryOff                                     uint32
	nameOff, args, deferreturn                   uint32
	pcsp, pcfile, pcln, npcdata, cuOffset        uint32
	startLine                                    uint32
	funcID, flag, nfuncdata                      uint8
	pcdata, funcdata                             []uint32
	name                                         string
}

func (d *differ) sides() [2]*side {
	if d.sh == nil {
		d.sh = &side{name: "hlink", m: d.H, img: d.H.img}
		d.ss = &side{name: "stock", m: d.S, img: d.S.img}
		for _, sd := range []*side{d.sh, d.ss} {
			sd.readModuledata()
		}
	}
	return [2]*side{d.sh, d.ss}
}

func (sd *side) readModuledata() {
	i := sd.m.named("runtime.firstmoduledata")
	if i < 0 {
		fatalf("%s: runtime.firstmoduledata not in the map", sd.name)
	}
	sd.mdAt, sd.mdSz = sd.m.syms[i].addr, sd.m.syms[i].size
	sd.md = map[string][]uint64{}
	a := sd.mdAt
	for _, f := range mdLayout {
		var w []uint64
		for k := 0; k < f.words; k++ {
			w = append(w, sd.img.u64(a))
			a += 8
		}
		sd.md[f.name] = w
	}
	if sd.mdSz > a-sd.mdAt {
		sd.tail = sd.img.bytes(a, sd.mdSz-(a-sd.mdAt))
	}
}

func (sd *side) f(name string) uint64 { return sd.md[name][0] }

// cstrings: the NUL-terminated strings of a moduledata []byte table, counted.
func (sd *side) cstrings(field string) map[string]int {
	t := sd.md[field]
	b := sd.img.view(t[0], t[1])
	r := map[string]int{}
	for _, x := range bytes.Split(b, []byte{0}) {
		if len(x) > 0 {
			r[string(x)]++
		}
	}
	return r
}

var fipsMarker = map[string]bool{"go:textfipsstart": true, "go:textfipsend": true}

func (sd *side) cstring(a uint64) string {
	var b []byte
	for k := 0; k < 1<<16; k++ {
		c := sd.img.u8(a + uint64(k))
		if c == 0 {
			break
		}
		b = append(b, c)
	}
	return string(b)
}

// ---- pclntab

func (sd *side) readPclntab() {
	im := sd.img
	ftab := sd.md["ftab"]
	sd.nfunc = int(ftab[1]) - 1
	if sd.nfunc <= 0 || sd.nfunc > 1<<24 {
		fatalf("%s: implausible ftab length %d", sd.name, ftab[1])
	}
	text := sd.f("text")
	pclntable := sd.md["pclntable"][0]
	sd.byEntry = map[uint64]int{}
	sd.funcs = make([]fn, sd.nfunc)
	for i := 0; i < sd.nfunc; i++ {
		e := ftab[0] + 8*uint64(i)
		f := &sd.funcs[i]
		f.entryOff = im.u32(e)
		f.entry = text + uint64(f.entryOff)
		f.end = text + uint64(im.u32(e+8))
		f.funcAddr = pclntable + uint64(im.u32(e+4))
		p := im.bytes(f.funcAddr, 44)
		if p == nil {
			continue
		}
		u := func(o int) uint32 { return le.Uint32(p[o:]) }
		f.nameOff, f.args, f.deferreturn = u(4), u(8), u(12)
		f.pcsp, f.pcfile, f.pcln, f.npcdata, f.cuOffset, f.startLine = u(16), u(20), u(24), u(28), u(32), u(36)
		f.funcID, f.flag, f.nfuncdata = p[40], p[41], p[43]
		if f.npcdata > 1024 {
			f.npcdata = 0
		}
		for k := uint32(0); k < f.npcdata; k++ {
			f.pcdata = append(f.pcdata, im.u32(f.funcAddr+44+4*uint64(k)))
		}
		for k := uint32(0); k < uint32(f.nfuncdata); k++ {
			f.funcdata = append(f.funcdata, im.u32(f.funcAddr+44+4*uint64(f.npcdata)+4*uint64(k)))
		}
		f.name = sd.cstring(sd.md["funcnametab"][0] + uint64(f.nameOff))
		sd.byEntry[f.entry] = i
		if u(0) != f.entryOff {
			rep.diff("pclntab", sd.name+": _func.entryOff is not its ftab entry", "%s: ftab %#x, _func %#x", f.name, f.entryOff, u(0))
		}
	}
}

// pcvalue: the table at pctab offset off: its bytes (through the terminator) and its (pc end, value) runs.
type pcrun struct {
	pc  uint64
	val int32
}

func (sd *side) pcvalue(off uint32) ([]byte, []pcrun) {
	if off == 0 {
		return nil, nil
	}
	base := sd.md["pctab"][0]
	n := sd.md["pctab"][1]
	if uint64(off) >= n {
		return nil, nil
	}
	p := sd.img.view(base+uint64(off), n-uint64(off))
	val, pc := int32(-1), uint64(0)
	var runs []pcrun
	k := 0
	for first := true; k < len(p); first = false {
		uvdelta, n1 := binary.Uvarint(p[k:])
		if n1 <= 0 {
			break
		}
		if uvdelta == 0 && !first {
			k++
			break
		}
		k += n1
		uv := uint32(uvdelta)
		val += int32(-(uv & 1) ^ (uv >> 1))
		pcdelta, n2 := binary.Uvarint(p[k:])
		if n2 <= 0 {
			break
		}
		k += n2
		pc += pcdelta // PCQuantum 1 on amd64
		runs = append(runs, pcrun{pc, val})
	}
	return p[:k], runs
}

func (sd *side) fileName(cuOff uint32, idx int32) string {
	if idx < 0 {
		return fmt.Sprintf("<file %d>", idx)
	}
	cut := sd.md["cutab"]
	k := uint64(cuOff) + uint64(idx)
	if k >= cut[1] {
		return fmt.Sprintf("<cutab %d out of range>", k)
	}
	fo := sd.img.u32(cut[0] + 4*k)
	if fo == ^uint32(0) {
		return "<no file>"
	}
	return sd.cstring(sd.md["filetab"][0] + uint64(fo))
}

// findfunc: runtime/symtab.go findfunc, on the decoded tables; the function index, or -1.
func (sd *side) findfunc(pc uint64) int {
	im := sd.img
	text, minpc := sd.f("text"), sd.f("minpc")
	if pc < minpc || pc >= sd.f("maxpc") {
		return -1
	}
	pcOff := uint32(pc - text)
	x := uint64(pcOff) + text - minpc
	b := x / abi.FuncTabBucketSize
	i := x % abi.FuncTabBucketSize / (abi.FuncTabBucketSize / 16)
	ffb := sd.f("findfunctab") + b*20
	idx := im.u32(ffb) + uint32(im.u8(ffb+4+i))
	ftab := sd.md["ftab"][0]
	for n := 0; ; n++ {
		if int(idx)+1 > sd.nfunc || n > 1<<20 {
			return -1
		}
		if im.u32(ftab+8*uint64(idx+1)) > pcOff {
			break
		}
		idx++
	}
	return int(idx)
}

func (d *differ) checkPclntab() {
	sds := d.sides()
	H, S := sds[0], sds[1]
	st := rep.stat("pclntab")
	for _, sd := range sds {
		sd.readPclntab()
		// functab sorted, findfunctab consistent
		for i := range sd.funcs {
			f := &sd.funcs[i]
			if i > 0 && f.entry <= sd.funcs[i-1].entry {
				rep.diff("pclntab", sd.name+": functab not sorted by entry", "%d %s @%#x after %s @%#x", i, f.name, f.entry, sd.funcs[i-1].name, sd.funcs[i-1].entry)
			}
			if f.end <= f.entry {
				continue
			}
			for _, pc := range []uint64{f.entry, f.end - 1} {
				if j := sd.findfunc(pc); j != i {
					name := "?"
					if j >= 0 {
						name = sd.funcs[j].name
					}
					rep.diff("pclntab", sd.name+": findfunctab lookup gives another function", "pc %#x in %s (index %d): found %d (%s)", pc, f.name, i, j, name)
				}
			}
			// the function's name is its symbol's
			ok := false
			for _, k := range sd.m.at(f.entry) {
				if sd.m.syms[k].class == cText && sd.m.syms[k].name == f.name {
					ok = true
				}
			}
			if !ok && sd == d.ss && fipsMarker[f.name] {
				rep.allow("F1", "%s @%#x", f.name, f.entry)
			} else if !ok {
				rep.diff("pclntab", sd.name+": function name is not the name of the text symbol at its entry", "%s @%#x (%s)", f.name, f.entry, d.desc(sd.m, f.entry))
			}
		}
		if last := sd.funcs[len(sd.funcs)-1]; last.end < last.entry {
			rep.diff("pclntab", sd.name+": end of the last function before its entry", "%s", last.name)
		}
	}
	// pctab: the bytes the functions reference
	for _, sd := range sds {
		ref := map[uint32]int{}
		for i := range sd.funcs {
			f := &sd.funcs[i]
			for _, o := range append([]uint32{f.pcsp, f.pcfile, f.pcln}, f.pcdata...) {
				if _, ok := ref[o]; !ok && o != 0 {
					b, _ := sd.pcvalue(o)
					ref[o] = len(b)
				}
			}
		}
		offs := make([]uint32, 0, len(ref))
		for o := range ref {
			offs = append(offs, o)
		}
		sort.Slice(offs, func(i, j int) bool { return offs[i] < offs[j] })
		covered, end := uint64(0), uint64(0)
		for _, o := range offs {
			a, b := uint64(o), uint64(o)+uint64(ref[o])
			if b > end {
				covered += b - max(a, end)
				end = b
			}
		}
		total := sd.md["pctab"][1]
		sd.pctabUnref = total - covered
		sd.pctabRefd = covered
	}
	if H.pctabRefd == S.pctabRefd && H.pctabUnref < 8+1 && S.pctabUnref < 8+1 {
		if H.pctabUnref != S.pctabUnref {
			rep.allow("P1", "pctab: %d bytes referenced in both; unreferenced: hlink %d, stock %d", H.pctabRefd, H.pctabUnref, S.pctabUnref)
		}
	} else {
		rep.diff("pclntab", "pctab: bytes referenced by functions / not referenced differ", "hlink %d / %d, stock %d / %d", H.pctabRefd, H.pctabUnref, S.pctabRefd, S.pctabUnref)
	}
	// function by function
	seen := make([]bool, len(S.funcs))
	for i := range H.funcs {
		hf := &H.funcs[i]
		st.compared++
		sfi := -1
		for _, k := range H.m.at(hf.entry) {
			if hs := &H.m.syms[k]; hs.class == cText && hs.name == hf.name && hs.match >= 0 {
				if j, ok := S.byEntry[S.m.syms[hs.match].addr]; ok {
					sfi = j
				}
			}
		}
		if sfi < 0 {
			st.unmatched++
			rep.diff("pclntab", "function only in hlink's functab", "%s @%#x", hf.name, hf.entry)
			continue
		}
		seen[sfi] = true
		if d.compareFunc(H, S, hf, &S.funcs[sfi]) {
			st.equal++
		} else {
			st.different++
		}
	}
	for j := range S.funcs {
		if !seen[j] && fipsMarker[S.funcs[j].name] {
			st.unmatched++
			st.allowed++
			d.nFipsMarkers++
		} else if !seen[j] {
			st.unmatched++
			rep.diff("pclntab", "function only in the stock functab", "%s @%#x", S.funcs[j].name, S.funcs[j].entry)
		}
	}
}

func (d *differ) compareFunc(H, S *side, hf, sf *fn) bool {
	ok := true
	bad := func(kind, format string, args ...any) {
		ok = false
		rep.diff("pclntab", kind, "%s: "+format, append([]any{hf.name}, args...)...)
	}
	if hf.name != sf.name {
		bad("name", "stock %q", sf.name)
	}
	if hf.args != sf.args {
		bad("args", "hlink %d, stock %d", hf.args, sf.args)
	}
	if hf.deferreturn != sf.deferreturn {
		bad("deferreturn", "hlink %#x, stock %#x", hf.deferreturn, sf.deferreturn)
	}
	if hf.startLine != sf.startLine {
		bad("startLine", "hlink %d, stock %d", int32(hf.startLine), int32(sf.startLine))
	}
	if hf.funcID != sf.funcID {
		bad("funcID", "hlink %d, stock %d", hf.funcID, sf.funcID)
	}
	if hf.flag != sf.flag {
		bad("flag", "hlink %#x, stock %#x", hf.flag, sf.flag)
	}
	if hf.npcdata != sf.npcdata {
		bad("npcdata", "hlink %d, stock %d", hf.npcdata, sf.npcdata)
	}
	if hf.nfuncdata != sf.nfuncdata {
		bad("nfuncdata", "hlink %d, stock %d", hf.nfuncdata, sf.nfuncdata)
	}
	if (hf.end - hf.entry) != (sf.end - sf.entry) {
		// the extent of a function in functab includes the padding to the next one, which depends on the
		// layout; but it must cover the function's symbol
		hs := H.m.containing(hf.entry)
		if hs >= 0 && hf.end-hf.entry < H.m.syms[hs].size {
			bad("functab extent shorter than the function", "hlink %d bytes, symbol %d", hf.end-hf.entry, H.m.syms[hs].size)
		}
	}
	cmpTab := func(what string, ho, so uint32) {
		hb, _ := H.pcvalue(ho)
		sb, _ := S.pcvalue(so)
		if !bytes.Equal(hb, sb) {
			bad(what+" table differs", "hlink %d bytes @%d, stock %d bytes @%d", len(hb), ho, len(sb), so)
		}
	}
	cmpTab("pcsp", hf.pcsp, sf.pcsp)
	cmpTab("pcln", hf.pcln, sf.pcln)
	for k := 0; k < len(hf.pcdata) && k < len(sf.pcdata); k++ {
		cmpTab(fmt.Sprintf("pcdata[%d]", k), hf.pcdata[k], sf.pcdata[k])
	}
	// pcfile: as file names
	_, hr := H.pcvalue(hf.pcfile)
	_, sr := S.pcvalue(sf.pcfile)
	if len(hr) != len(sr) {
		bad("pcfile differs", "hlink %d runs, stock %d", len(hr), len(sr))
	} else {
		for k := range hr {
			hn, sn := H.fileName(hf.cuOffset, hr[k].val), S.fileName(sf.cuOffset, sr[k].val)
			if hr[k].pc != sr[k].pc || hn != sn {
				bad("pcfile differs", "run %d: hlink pc %#x %s, stock pc %#x %s", k, hr[k].pc, hn, sr[k].pc, sn)
				break
			}
		}
	}
	// funcdata
	for k := 0; k < len(hf.funcdata) && k < len(sf.funcdata); k++ {
		ho, so := hf.funcdata[k], sf.funcdata[k]
		if (ho == ^uint32(0)) != (so == ^uint32(0)) {
			bad(fmt.Sprintf("funcdata[%d] present on one side only", k), "hlink %#x, stock %#x", ho, so)
			continue
		}
		if ho == ^uint32(0) {
			continue
		}
		ha, sa := H.f("gofunc")+uint64(ho), S.f("gofunc")+uint64(so)
		if k == abi.FUNCDATA_InlTree && len(hf.pcdata) > abi.PCDATA_InlTreeIndex && len(sf.pcdata) > abi.PCDATA_InlTreeIndex {
			_, hr := H.pcvalue(hf.pcdata[abi.PCDATA_InlTreeIndex])
			_, sr := S.pcvalue(sf.pcdata[abi.PCDATA_InlTreeIndex])
			n := int32(-1)
			for _, r := range append(hr, sr...) {
				n = max(n, r.val)
			}
			for e := int32(0); e <= n; e++ {
				he, se := H.inlEntry(ha, e), S.inlEntry(sa, e)
				if he != se {
					bad("inline tree entry differs", "entry %d: hlink %+v, stock %+v", e, he, se)
					break
				}
			}
			continue
		}
		hk, sk := d.funcdataSym(H, ha), d.funcdataSym(S, sa)
		switch {
		case hk < 0 || sk < 0:
			bad(fmt.Sprintf("funcdata[%d] points at no symbol", k), "hlink %s, stock %s", d.desc(H.m, ha), d.desc(S.m, sa))
		case H.m.syms[hk].match == sk:
		case H.m.syms[hk].name != "" && H.m.syms[hk].name == S.m.syms[sk].name:
		case H.m.syms[hk].size == S.m.syms[sk].size && bytes.Equal(H.img.view(ha, H.m.syms[hk].size), S.img.view(sa, S.m.syms[sk].size)):
		default:
			bad(fmt.Sprintf("funcdata[%d] points at different data", k), "hlink %s, stock %s", d.desc(H.m, ha), d.desc(S.m, sa))
		}
	}
	return ok
}

func (d *differ) funcdataSym(sd *side, a uint64) int32 {
	for _, k := range sd.m.at(a) {
		if c := sd.m.syms[k].class; c == cFuncdata || c == cRodata {
			return k
		}
	}
	return -1
}

type inlEntry struct {
	FuncID    uint8
	Name      string
	ParentPC  int32
	StartLine int32
}

func (sd *side) inlEntry(a uint64, k int32) inlEntry {
	e := a + 16*uint64(k)
	return inlEntry{sd.img.u8(e), sd.cstring(sd.md["funcnametab"][0] + uint64(sd.img.u32(e+4))), int32(sd.img.u32(e + 8)), int32(sd.img.u32(e + 12))}
}

// ---- moduledata

func (d *differ) checkModuledata() {
	sds := d.sides()
	st := rep.stat("moduledata")
	check := func(sd *side, ok bool, kind, format string, args ...any) {
		st.compared++
		if ok {
			st.equal++
			return
		}
		st.different++
		rep.diff("moduledata", sd.name+": "+kind, format, args...)
	}
	for _, sd := range sds {
		m := sd.m
		addr := func(name string) uint64 {
			a, ok := m.addrOf(name)
			if !ok {
				rep.diff("moduledata", sd.name+": symbol missing from the map", "%s", name)
			}
			return a
		}
		eqSym := func(field, name string) {
			v, a := sd.f(field), addr(name)
			check(sd, v == a, "field is not its symbol's address", "%s = %#x, %s @%#x", field, v, name, a)
		}
		slice := func(field string) []uint64 {
			s := sd.md[field]
			check(sd, s[1] == s[2], "slice len != cap", "%s len %d cap %d", field, s[1], s[2])
			return s
		}
		eqSym("pcHeader", "runtime.pcheader")
		for f, n := range map[string]string{"funcnametab": "runtime.funcnametab", "cutab": "runtime.cutab", "filetab": "runtime.filetab",
			"pctab": "runtime.pctab", "pclntable": "runtime.functab", "ftab": "runtime.functab"} {
			s := slice(f)
			a := addr(n)
			check(sd, s[0] == a, "slice does not start at its symbol", "%s @%#x, %s @%#x", f, s[0], n, a)
		}
		// the pcHeader
		ph := sd.f("pcHeader")
		im := sd.img
		check(sd, im.u32(ph) == uint32(abi.CurrentPCLnTabMagic) && im.u8(ph+6) == 1 && im.u8(ph+7) == 8, "pcHeader magic/minLC/ptrSize", "%#x %d %d", im.u32(ph), im.u8(ph+6), im.u8(ph+7))
		check(sd, im.u64(ph+8)+1 == sd.md["ftab"][1], "pcHeader nfunc != len(ftab)-1", "nfunc %d, ftab %d", im.u64(ph+8), sd.md["ftab"][1])
		for k, f := range []string{"funcnametab", "cutab", "filetab", "pctab", "pclntable"} {
			o := im.u64(ph + 32 + 8*uint64(k))
			check(sd, ph+o == sd.md[f][0], "pcHeader offset", "%s: pcHeader+%#x = %#x, moduledata %#x", f, o, ph+o, sd.md[f][0])
		}
		nfiles := im.u64(ph + 16)
		_ = nfiles
		eqSym("findfunctab", "runtime.findfunctab")
		ftab := sd.md["ftab"]
		nf := ftab[1] - 1
		text := sd.f("text")
		check(sd, sd.f("minpc") == text+uint64(im.u32(ftab[0])), "minpc is not the first function's entry", "minpc %#x", sd.f("minpc"))
		check(sd, sd.f("maxpc") == text+uint64(im.u32(ftab[0]+8*nf)), "maxpc is not the end of the function table", "maxpc %#x", sd.f("maxpc"))
		for _, f := range []string{"text", "etext", "noptrdata", "enoptrdata", "data", "edata", "bss", "ebss", "noptrbss", "enoptrbss",
			"covctrs", "ecovctrs", "end", "gcdata", "gcbss", "types", "etypes", "rodata", "epclntab"} {
			eqSym(f, "runtime."+f)
		}
		eqSym("gofunc", "go:func.*")
		// every symbol of a class inside its range
		type rng struct {
			lo, hi string
		}
		ranges := map[uint8]rng{cText: {"text", "etext"}, cNoptrdata: {"noptrdata", "enoptrdata"}, cData: {"data", "edata"},
			cBss: {"bss", "ebss"}, cNoptrbss: {"noptrbss", "enoptrbss"}, cTypes: {"types", "etypes"}, cItabs: {"types", "etypes"},
			cFuncdata: {"gofunc", "epclntab"}}
		out := map[uint8]int{}
		var ex []string
		for i := range m.syms {
			s := &m.syms[i]
			r, ok := ranges[s.class]
			if !ok {
				continue
			}
			lo, hi := sd.f(r.lo), sd.f(r.hi)
			if s.class == cFuncdata && sd == d.ss && s.name == "" {
				// (stock inline trees are in go:func.* too)
			}
			if s.addr < lo || s.addr+s.size > hi {
				out[s.class]++
				if len(ex) < *flagN {
					ex = append(ex, fmt.Sprintf("%s %s @%#x+%d not in [%s %#x, %s %#x)", className[s.class], s.name, s.addr, s.size, r.lo, lo, r.hi, hi))
				}
			}
		}
		st.compared++
		if len(out) == 0 {
			st.equal++
		} else {
			st.different++
			for _, e := range ex {
				rep.diff("moduledata", sd.name+": symbol outside the moduledata range of its class", "%s", e)
			}
		}
		// the itabs in [types+itaboffset, +itabsize), typelinks in [types, types+typedesclen)
		tsm := sd.md["textsectmap"]
		check(sd, tsm[1] == 1 && tsm[2] == 1, "textsectmap has more than one section", "len %d", tsm[1])
		eqSym("textsectmap", "runtime.textsectionmap")
		var textLen uint64
		if sc := sd.img.f.Section(".text"); sc != nil {
			textLen = sc.Size
		}
		check(sd, im.u64(tsm[0]) == 0 && im.u64(tsm[0]+8) == textLen && im.u64(tsm[0]+16) == text,
			"textsectmap[0] is not {0, length of .text, text}", "{%#x, %#x, %#x}, .text %#x", im.u64(tsm[0]), im.u64(tsm[0]+8), im.u64(tsm[0]+16), textLen)
		zero := func(f string) {
			z := true
			for _, w := range sd.md[f] {
				z = z && w == 0
			}
			check(sd, z, "field not zero", "%s = %#x", f, sd.md[f])
		}
		for _, f := range []string{"ptab", "pluginpath", "pkghashes", "modulename", "modulehashes"} {
			zero(f)
		}
		it := slice("inittasks")
		if it[1] > 0 {
			eqSym("inittasks", "go:main.inittasks")
		}
		check(sd, len(sd.tail) > 1 && sd.tail[0] == 1 && sd.tail[1] == 0, "hasmain != 1 or bad != 0", "% x", sd.tail[:min(2, len(sd.tail))])
		allz := true
		for _, b := range sd.tail[min(2, len(sd.tail)):] {
			allz = allz && b == 0
		}
		check(sd, allz, "moduledata fields built at run time are not zero", "% x", sd.tail)
	}
	// across the two
	H, S := sds[0], sds[1]
	cross := func(f string, k int) {
		hv, sv := H.md[f][k], S.md[f][k]
		st.compared++
		if hv == sv {
			st.equal++
			return
		}
		st.different++
		rep.diff("moduledata", "field differs between the binaries", "%s[%d]: hlink %d, stock %d", f, k, hv, sv)
	}
	st.compared++
	if H.md["ftab"][1]+uint64(d.nFipsMarkers) == S.md["ftab"][1] {
		st.equal++
		if d.nFipsMarkers > 0 {
			rep.allow("F1", "ftab: hlink %d functions, stock %d", H.md["ftab"][1]-1, S.md["ftab"][1]-1)
		}
	} else {
		st.different++
		rep.diff("moduledata", "field differs between the binaries", "ftab len: hlink %d, stock %d (%d FIPS markers)", H.md["ftab"][1], S.md["ftab"][1], d.nFipsMarkers)
	}
	cross("textsectmap", 1)
	cross("inittasks", 1)
	cross("itabsize", 0)
	cross("typedesclen", 0)
	// file names: the stock linker lists the files the live functions use, hlink may list more (rule F2)
	hf, sf := H.cstrings("filetab"), S.cstrings("filetab")
	for n := range sf {
		st.compared++
		if hf[n] > 0 {
			st.equal++
		} else {
			st.different++
			rep.diff("moduledata", "file name in the stock filetab only", "%q", n)
		}
	}
	for n := range hf {
		if sf[n] == 0 {
			rep.allow("F2", "file %q", n)
		}
	}
	if hn, sn := H.img.u64(H.f("pcHeader")+16), S.img.u64(S.f("pcHeader")+16); hn != sn || H.md["cutab"][1] != S.md["cutab"][1] {
		rep.allow("F2", "nfiles: hlink %d, stock %d; cutab: hlink %d entries, stock %d", hn, sn, H.md["cutab"][1], S.md["cutab"][1])
	}
	// function names: the same names (rule F1: the stock FIPS markers)
	hn, sn := H.cstrings("funcnametab"), S.cstrings("funcnametab")
	for _, x := range []struct {
		a, b   map[string]int
		who    string
	}{{hn, sn, "hlink"}, {sn, hn, "stock"}} {
		for n, c := range x.a {
			if x.b[n] == c {
				continue
			}
			if x.who == "stock" && fipsMarker[n] && x.b[n] == 0 {
				rep.allow("F1", "funcnametab: %s", n)
				continue
			}
			st.different++
			rep.diff("moduledata", "funcnametab: name count differs", "%q: %s %d times, the other %d", n, x.who, c, x.b[n])
		}
	}
	for _, f := range []string{"pctab"} {
		hv, sv := H.md[f][1], S.md[f][1]
		st.compared++
		if hv == sv {
			st.equal++
		} else if (hv+7)&^7 == (sv+7)&^7 {
			st.allowed++
			rep.allow("P1", "%s: hlink %d bytes, stock %d", f, hv, sv)
		} else {
			st.different++
			rep.diff("moduledata", "table size differs between the binaries", "%s: hlink %d bytes, stock %d", f, hv, sv)
		}
	}
	// the init tasks, in order
	names := func(sd *side) []string {
		it := sd.md["inittasks"]
		var r []string
		for k := uint64(0); k < it[1] && k < 1<<20; k++ {
			r = append(r, d.symAt(sd.m, sd.img.u64(it[0]+8*k)))
		}
		return r
	}
	hl, sl := names(H), names(S)
	st.compared++
	if fmt.Sprint(hl) == fmt.Sprint(sl) {
		st.equal++
	} else {
		st.different++
		rep.diff("moduledata", "init tasks differ (by name, in order)", "hlink %v\n        stock %v", hl, sl)
	}
}

// symAt: the name of the (non-carrier, non-boundary) symbol at address a.
func (d *differ) symAt(m *lmap, a uint64) string {
	for _, i := range m.at(a) {
		if c := m.syms[i].class; c != cCarrier && c != cBoundary && m.syms[i].size > 0 {
			return m.syms[i].name
		}
	}
	return fmt.Sprintf("<%#x>", a)
}

// ---- the symbols the linker writes

func (d *differ) checkLinkerSyms() {
	sds := d.sides()
	H, S := sds[0], sds[1]
	st := rep.stat("linker symbols")
	result := func(ok bool, kind, format string, args ...any) {
		st.compared++
		if ok {
			st.equal++
		} else {
			st.different++
			rep.diff("linker symbols", kind, format, args...)
		}
	}
	str := func(sd *side, name string) (string, bool) {
		a, ok := sd.m.addrOf(name)
		if !ok {
			return "", false
		}
		p, n := sd.img.u64(a), sd.img.u64(a+8)
		if n > 1<<24 {
			return "", false
		}
		return string(sd.img.bytes(p, n)), true
	}
	done := map[string]bool{"runtime.firstmoduledata": true}
	for i := range H.m.syms {
		h := &H.m.syms[i]
		if h.class != cLinker || done[h.name] {
			continue
		}
		done[h.name] = true
		name := h.name
		switch {
		case H.m.named(name+".str") >= 0:
			// a string variable the linker sets: runtime.buildVersion, runtime.modinfo, -X
			hs, ok1 := str(H, name)
			ss, ok2 := str(S, name)
			done[name+".str"] = true
			result(ok1 && ok2 && hs == ss, "string variable differs", "%s: hlink %q, stock %q", name, hs, ss)
		case len(name) > 4 && name[len(name)-4:] == ".str":
			// (with its variable)
		case name == "runtime.lastmoduledatap":
			for _, sd := range sds {
				a, _ := sd.m.addrOf(name)
				result(sd.img.u64(a) == sd.mdAt, sd.name+": runtime.lastmoduledatap does not point at firstmoduledata", "%#x", sd.img.u64(a))
			}
		case name == "go:main.inittasks" || name == "go:runtime.inittasks":
			list := func(sd *side) []string {
				k := sd.m.named(name)
				if k < 0 {
					return nil
				}
				s := &sd.m.syms[k]
				var r []string
				for o := uint64(0); o+8 <= s.size; o += 8 {
					r = append(r, d.symAt(sd.m, sd.img.u64(s.addr+o)))
				}
				return r
			}
			hl, sl := list(H), list(S)
			result(fmt.Sprint(hl) == fmt.Sprint(sl), "init task list differs", "%s: hlink %v\n        stock %v", name, hl, sl)
		case name == "runtime.runtime_inittasks":
			for _, sd := range sds {
				a, _ := sd.m.addrOf(name)
				t, _ := sd.m.addrOf("go:runtime.inittasks")
				k := sd.m.named("go:runtime.inittasks")
				n := uint64(0)
				if k >= 0 {
					n = sd.m.syms[k].size / 8
				}
				result(sd.img.u64(a) == t && sd.img.u64(a+8) == n && sd.img.u64(a+16) == n, sd.name+": runtime_inittasks is not the slice of go:runtime.inittasks",
					"{%#x %d %d}, list @%#x (%d)", sd.img.u64(a), sd.img.u64(a+8), sd.img.u64(a+16), t, n)
			}
		case name == "runtime.textsectionmap":
			// checked with moduledata
		case name == "go:fipsinfo":
			for _, sd := range sds {
				d.checkFipsinfo(sd, result)
			}
		default:
			// byte for byte: go:buildinfo, runtime.disableMemoryProfiling, ...
			k := S.m.named(name)
			if k < 0 {
				continue // (rule L1, counted with the symbol sets)
			}
			s := &S.m.syms[k]
			hb, sb := H.img.bytes(h.addr, h.size), S.img.bytes(s.addr, s.size)
			result(bytes.Equal(hb, sb), "linker-made symbol differs", "%s: hlink % x\n        stock % x", name, hb[:min(64, len(hb))], sb[:min(64, len(sb))])
		}
	}
	// the build id note
	hn, sn := noteBytes(H.img), noteBytes(S.img)
	result(bytes.Equal(hn, sn), "Go build id note differs", "hlink %q, stock %q", hn, sn)
}

func noteBytes(im *image) []byte {
	for _, s := range im.f.Sections {
		if s.Name == ".note.go.buildid" {
			b, _ := s.Data()
			return b
		}
	}
	return nil
}

func (d *differ) checkFipsinfo(sd *side, result func(bool, string, string, ...any)) {
	a, ok := sd.m.addrOf("go:fipsinfo")
	if !ok {
		result(false, sd.name+": go:fipsinfo missing", "")
		return
	}
	im := sd.img
	result(string(im.bytes(a, 16)) == "\xff Go fipsinfo \xff\x00", sd.name+": go:fipsinfo magic", "%q", im.bytes(a, 16))
	result(im.u64(a+48) == a, sd.name+": go:fipsinfo self pointer", "%#x, @%#x", im.u64(a+48), a)
	names := []string{"go:textfipsstart", "go:textfipsend", "go:rodatafipsstart", "go:rodatafipsend",
		"go:noptrdatafipsstart", "go:noptrdatafipsend", "go:datafipsstart", "go:datafipsend"}
	var b [8]uint64
	for i, n := range names {
		b[i] = im.u64(a + 56 + 8*uint64(i))
		v, ok := sd.m.addrOf(n)
		result(ok && v == b[i], sd.name+": go:fipsinfo bound is not its symbol's address", "%s: %#x, map %#x", n, b[i], v)
	}
	hm := hmac.New(sha256.New, make([]byte, 32))
	hm.Write([]byte("go fips object v1\n"))
	for i := 0; i < 8; i += 2 {
		var n [8]byte
		binary.BigEndian.PutUint64(n[:], b[i+1]-b[i])
		hm.Write(n[:])
		hm.Write(im.bytes(b[i], b[i+1]-b[i]))
	}
	sum := hm.Sum(nil)
	result(bytes.Equal(sum, im.bytes(a+16, 32)), sd.name+": go:fipsinfo hash is not the hash of the FIPS sections", "stored % x, computed % x", im.bytes(a+16, 32), sum)
}

// ---- 6. GC programs of data and bss

// runGCProg expands a GC program (runtime/mbitmap.go runGCProg) into one bit per word, at most max bits.
func runGCProg(im *image, a uint64, max uint64) ([]bool, error) {
	var bits []bool
	rd := func() byte { b := im.u8(a); a++; return b }
	varint := func() uint64 {
		var v uint64
		for off := uint(0); ; off += 7 {
			x := rd()
			v |= uint64(x&0x7f) << off
			if x&0x80 == 0 || off > 63 {
				return v
			}
		}
	}
	for steps := 0; ; steps++ {
		if uint64(len(bits)) > max || steps > 1<<26 {
			return bits, fmt.Errorf("program longer than the section")
		}
		inst := rd()
		n := uint64(inst & 0x7f)
		if inst&0x80 == 0 {
			if n == 0 {
				return bits, nil
			}
			for i := uint64(0); i < n; i += 8 {
				x := rd()
				for k := uint64(0); k < 8 && i+k < n; k++ {
					bits = append(bits, x>>k&1 != 0)
				}
			}
			continue
		}
		if n == 0 {
			n = varint()
		}
		c := varint()
		if n > uint64(len(bits)) || n*c > max {
			return bits, fmt.Errorf("bad repeat %d x %d at bit %d", n, c, len(bits))
		}
		pat := bits[len(bits)-int(n):]
		pat = append([]bool(nil), pat...)
		for k := uint64(0); k < c; k++ {
			bits = append(bits, pat...)
		}
	}
}

func (d *differ) checkGC() {
	sds := d.sides()
	st := rep.stat("gc bitmaps")
	for _, sec := range []struct {
		prog, lo, hi string
		class        uint8
	}{{"gcdata", "data", "edata", cData}, {"gcbss", "bss", "ebss", cBss}} {
		var bm [2][]bool
		for k, sd := range sds {
			nw := (sd.f(sec.hi) - sd.f(sec.lo)) / 8
			b, err := runGCProg(sd.img, sd.f(sec.prog), nw+64)
			if err != nil {
				rep.diff("gc bitmaps", sd.name+": GC program does not decode", "%s: %v", sec.prog, err)
			}
			if uint64(len(b)) < nw {
				rep.diff("gc bitmaps", sd.name+": GC program shorter than its section", "%s: %d bits for %d words", sec.prog, len(b), nw)
				b = append(b, make([]bool, nw-uint64(len(b)))...)
			}
			bm[k] = b
			// every pointer bit is in a symbol of the section's class
			stray := 0
			for w := uint64(0); w < nw; w++ {
				if !b[w] {
					continue
				}
				a := sd.f(sec.lo) + 8*w
				i := sd.m.containing(a)
				if i < 0 || sd.m.syms[i].class != sec.class {
					stray++
					if stray <= *flagN {
						rep.diff("gc bitmaps", sd.name+": pointer bit outside every symbol of the section", "%s word %d (%s)", sec.prog, w, d.desc(sd.m, a))
					}
				}
			}
		}
		H, S := sds[0], sds[1]
		for i := range H.m.syms {
			h := &H.m.syms[i]
			if h.class != sec.class || h.match < 0 || h.size == 0 {
				continue
			}
			s := &S.m.syms[h.match]
			if s.class != sec.class {
				continue
			}
			st.compared++
			ho, so := h.addr-H.f(sec.lo), s.addr-S.f(sec.lo)
			if ho%8 != 0 || so%8 != 0 {
				// not word aligned: no word of it can be a pointer on either side
				if h.size < 8 {
					st.equal++
				} else {
					st.different++
					rep.diff("gc bitmaps", "symbol not word aligned", "%s: hlink @%#x, stock @%#x", h.name, h.addr, s.addr)
				}
				continue
			}
			ok := true
			for w := uint64(0); w < (h.size+7)/8; w++ {
				hb, sb := bm[0][ho/8+w], bm[1][so/8+w]
				if hb != sb {
					ok = false
					rep.diff("gc bitmaps", "pointer bit differs: "+className[sec.class], "%s word %d: hlink %v, stock %v", h.name, w, hb, sb)
					break
				}
			}
			if ok {
				st.equal++
			} else {
				st.different++
			}
		}
	}
}

// ---- 7. the types section: what the runtime walks

// descriptorSize: abi.Type.DescriptorSize of the type descriptor at a.
func descriptorSize(im *image, a uint64) (n int, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("%v", r)
		}
	}()
	buf := make([]uint64, 64)
	b := unsafe.Slice((*byte)(unsafe.Pointer(&buf[0])), 512)
	copy(b, im.bytes(a, 512))
	t := (*abi.Type)(unsafe.Pointer(&buf[0]))
	return t.DescriptorSize(), nil
}

// typeString: the string of the type descriptor at a (reflect's key for the sorted typelinks).
func (sd *side) typeString(a uint64) string {
	im := sd.img
	off := int32(im.u32(a + 40))
	n := sd.f("types") + uint64(off)
	l, k := binary.Uvarint(im.bytes(n+1, 10))
	s := string(im.bytes(n+1+uint64(k), l))
	if abi.TFlag(im.u8(a+20))&abi.TFlagExtraStar != 0 && len(s) > 0 {
		s = s[1:]
	}
	return s
}

func (d *differ) checkTypes() {
	sds := d.sides()
	st := rep.stat("types order")
	walked := [2]map[int32]bool{}
	itabs := [2]map[int32]bool{}
	for k, sd := range sds {
		walked[k], itabs[k] = map[int32]bool{}, map[int32]bool{}
		im := sd.img
		types := sd.f("types")
		td := types + 8
		end := types + sd.f("typedesclen")
		prev := ""
		bad := 0
		for td < end {
			td = (td + 7) &^ 7
			st.compared++
			ti := int32(-1)
			for _, i := range sd.m.at(td) {
				if sd.m.syms[i].class == cTypes {
					ti = i
				}
			}
			n, err := descriptorSize(im, td)
			if ti < 0 || err != nil {
				st.different++
				rep.diff("types order", sd.name+": the runtime's walk of the typelinks reaches no type", "@%#x (%s): %v", td, d.desc(sd.m, td), err)
				bad++
				break
			}
			walked[k][ti] = true
			ts := sd.typeString(td)
			ok := true
			if uint64(n) != sd.m.syms[ti].size {
				ok = false
				rep.diff("types order", sd.name+": DescriptorSize is not the symbol's size", "%s: %d, symbol %d", sd.m.syms[ti].name, n, sd.m.syms[ti].size)
			}
			if ts < prev {
				ok = false
				rep.diff("types order", sd.name+": typelinks not sorted by type string", "%q (%s) after %q", ts, sd.m.syms[ti].name, prev)
			}
			prev = ts
			if ok {
				st.equal++
			} else {
				st.different++
			}
			td += uint64(n)
		}
		if bad == 0 && td != end {
			rep.diff("types order", sd.name+": the typelinks walk does not end at typedesclen", "ends @%#x, types+typedesclen %#x", td, end)
		}
		// the itabs
		p := types + sd.f("itaboffset")
		iend := p + sd.f("itabsize")
		for p < iend {
			st.compared++
			ii := int32(-1)
			for _, i := range sd.m.at(p) {
				if sd.m.syms[i].class == cItabs {
					ii = i
				}
			}
			if ii < 0 {
				st.different++
				rep.diff("types order", sd.name+": the runtime's walk of the itabs reaches no itab", "@%#x (%s)", p, d.desc(sd.m, p))
				break
			}
			itabs[k][ii] = true
			size := uint64(32)
			if im.u64(p+24) != 0 {
				nm := im.u64(im.u64(p) + 64) // Inter.Methods len
				size = 32 + (nm-1)*8
			}
			if size != sd.m.syms[ii].size {
				st.different++
				rep.diff("types order", sd.name+": itab size is not the symbol's size", "%s: %d, symbol %d", sd.m.syms[ii].name, size, sd.m.syms[ii].size)
			} else {
				st.equal++
			}
			p += size
		}
		if p != iend {
			rep.diff("types order", sd.name+": the itab walk does not end at itaboffset+itabsize", "ends @%#x, want %#x", p, iend)
		}
		// every itab in the map was walked
		for i := range sd.m.syms {
			if sd.m.syms[i].class == cItabs && !itabs[k][int32(i)] {
				rep.diff("types order", sd.name+": itab the runtime does not see", "%s @%#x", sd.m.syms[i].name, sd.m.syms[i].addr)
			}
		}
	}
	// the same typelinks and itabs in both
	H, S := sds[0], sds[1]
	for _, x := range []struct {
		what string
		set  [2]map[int32]bool
	}{{"typelink", walked}, {"itab", itabs}} {
		var only []string
		for i := range x.set[0] {
			if m := H.m.syms[i].match; m < 0 || !x.set[1][m] {
				only = append(only, "hlink: "+H.m.syms[i].name)
			}
		}
		for i := range x.set[1] {
			if m := S.m.syms[i].match; m < 0 || !x.set[0][m] {
				only = append(only, "stock: "+S.m.syms[i].name)
			}
		}
		sort.Strings(only)
		for _, o := range only {
			rep.diff("types order", x.what+" seen by the runtime in one binary only", "%s", o)
		}
	}
	// hlink: what it calls typelinks is what the runtime walks
	for i := range H.m.syms {
		if H.m.syms[i].sect == "typelink" && !walked[0][int32(i)] {
			rep.diff("types order", "hlink: typelink-bucket type outside the runtime's walk", "%s", H.m.syms[i].name)
		}
	}
}

// ---- the ELF header

func (d *differ) checkELF() {
	sds := d.sides()
	st := rep.stat("elf")
	H, S := sds[0], sds[1]
	he, se := d.symAt(H.m, H.img.f.Entry), d.symAt(S.m, S.img.f.Entry)
	st.compared++
	if he == se {
		st.equal++
	} else {
		st.different++
		rep.diff("elf", "entry point", "hlink %s, stock %s", he, se)
	}
	for _, sd := range sds {
		for _, p := range sd.img.progs {
			st.compared++
			var want uint32
			switch {
			case p.Vaddr <= sd.f("text") && sd.f("text") < p.Vaddr+p.Memsz:
				want = uint32(1 | 4) // R X
			case p.Vaddr <= sd.f("rodata") && sd.f("rodata") < p.Vaddr+p.Memsz:
				want = 4
			case p.Vaddr <= sd.f("data") && sd.f("data") < p.Vaddr+p.Memsz:
				want = 4 | 2
			}
			if uint32(p.Flags) == want {
				st.equal++
			} else {
				st.different++
				rep.diff("elf", sd.name+": segment permissions", "PT_LOAD @%#x: %v", p.Vaddr, p.Flags)
			}
		}
	}
}
