// match.go: symbol sets, sizes and alignment, content and relocations.
package main

import (
	"bytes"
	"cmd/internal/objabi"
	"encoding/binary"
	"fmt"
	"sort"
)

var le = binary.LittleEndian

type differ struct {
	H, S   *lmap
	eqN    int32
	sh, ss *side

	nFipsMarkers int
	refd         map[string]bool

	pcfileBytesDiffer int
	pctabExplained    bool

	incoming map[int32][]int32 // (voteByReferences) relocations into hlink symbols that share their name, by target
}

func (d *differ) desc(m *lmap, a uint64) string {
	if i := m.containing(a); i >= 0 {
		s := &m.syms[i]
		if a == s.addr {
			return fmt.Sprintf("%s@%#x", s.name, a)
		}
		return fmt.Sprintf("%s+%#x@%#x", s.name, a-s.addr, a)
	}
	for _, i := range m.at(a) {
		if m.syms[i].class != cCarrier {
			return fmt.Sprintf("%s@%#x", m.syms[i].name, a)
		}
	}
	return fmt.Sprintf("%#x (no symbol)", a)
}

// ---- 1. symbol sets

func (d *differ) matchSymbols() {
	H, S := d.H, d.S
	st := rep.stat("symbols")
	names := make([]string, 0, len(H.byName))
	for n := range H.byName {
		names = append(names, n)
	}
	sort.Strings(names)
	var groups [][2][]int32
	for _, name := range names {
		hl := H.byName[name]
		var sl []int32
		for _, i := range S.byName[name] {
			if S.syms[i].class != cCarrier {
				sl = append(sl, i)
			}
		}
		if len(sl) == 0 {
			continue
		}
		if len(hl) == 1 && len(sl) == 1 {
			d.pair(hl[0], sl[0])
			continue
		}
		groups = append(groups, [2][]int32{hl, sl})
	}
	// names with several symbols: first the pairs the content and the relocation targets single out, then
	// the rest (whose targets may have been matched meanwhile)
	for round := 0; round < 3; round++ {
		for _, g := range groups {
			d.matchGroup(g[0], g[1], round < 2)
		}
	}
	d.matchByContent()
	for i := range H.syms {
		h := &H.syms[i]
		if h.match >= 0 {
			st.compared++
			if h.byContent {
				st.allowed++
				rep.allow("H1", "hlink %s = stock %s (%s, %d bytes)", h.name, S.syms[h.match].name, className[h.class], h.size)
			}
			if !classOK(h.class, S.syms[h.match].class) {
				st.different++
				rep.diff("symbols", "matched by name, but in different classes", "%s: hlink %s (%s), stock %s (%s)", h.name,
					className[h.class], h.sect, className[S.syms[h.match].class], S.syms[h.match].sect)
			} else {
				st.equal++
			}
			continue
		}
		st.unmatched++
		if d.allowOnlyHlink(h) {
			st.allowed++
			continue
		}
		rep.diff("symbols", "only in hlink: "+className[h.class], "%s (%s, %d bytes, @%#x)", h.name, h.sect, h.size, h.addr)
	}
	for i := range S.syms {
		s := &S.syms[i]
		if s.match >= 0 {
			continue
		}
		st.unmatched++
		if d.allowOnlyStock(s) {
			st.allowed++
			continue
		}
		rep.diff("symbols", "only in stock: "+className[s.class], "%s (%s, %d bytes, @%#x)", s.name, s.sect, s.size, s.addr)
	}
}

func classOK(h, s uint8) bool {
	switch {
	case h == s:
		return true
	case h == cBoundary && (s == cPclntab || s == cBoundary):
		return true // the pclntab pieces (rule P1)
	case h == cLinker && (s == cData || s == cNoptrdata || s == cRodata || s == cLinker):
		return true // string variables, init task lists, ... (checked by checkLinkerSyms)
	}
	return false
}

func (d *differ) pair(hi, si int32) {
	d.H.syms[hi].match = si
	d.S.syms[si].match = hi
}

// matchGroup matches symbols that share a name, by size, content with hlink's relocation sites masked,
// and the relocation targets. strict: only pairs where exactly one candidate fits, with every target known.
func (d *differ) matchGroup(hl, sl []int32, strict bool) {
	H, S := d.H, d.S
	big := len(hl)*len(sl) > 1<<14
	for _, hi := range hl {
		h := &H.syms[hi]
		if h.match >= 0 {
			continue
		}
		var eqs []int32 // candidates (matched or not) that fit
		unknown := false
		for _, si := range sl {
			s := &S.syms[si]
			if s.size != h.size || (big && s.match >= 0) {
				continue
			}
			if !d.sameMasked(hi, si) {
				continue
			}
			ok, unk := d.relocsAgree(hi, si)
			if !ok {
				continue
			}
			unknown = unknown || unk
			eqs = append(eqs, si)
			if big {
				break
			}
		}
		var free []int32
		for _, si := range eqs {
			if S.syms[si].match < 0 {
				free = append(free, si)
			}
		}
		pick := int32(-1)
		switch {
		case strict:
			if len(free) == 1 && len(eqs) == 1 && !unknown {
				pick = free[0]
			}
		case len(free) > 0:
			pick = free[0]
			if len(free) > 1 {
				if v := d.voteByReferences(hi, free); v >= 0 {
					pick = v
				}
			}
		default:
			// nothing fits: take the only unmatched one of the same size, if there is just one, so that its
			// differences get reported
			n := 0
			for _, si := range sl {
				if S.syms[si].match < 0 && S.syms[si].size == h.size {
					pick, n = si, n+1
				}
			}
			if n != 1 {
				pick = -1
			}
		}
		if pick < 0 {
			continue
		}
		d.pair(hi, pick)
		if len(eqs) > 1 {
			id := int32(0)
			for _, si := range eqs {
				if e := S.syms[si].eq; e != 0 {
					id = e
				}
			}
			if id == 0 {
				d.eqN++
				id = d.eqN
			}
			h.eq = id
			for _, si := range eqs {
				S.syms[si].eq = id
			}
		}
	}
}

// voteByReferences picks, among stock candidates that fit hlink symbol hi equally (same name, size and bytes:
// e.g. two packages' file-local assembly functions of one name), the one the stock binary's references point
// at from the counterparts of hi's matched referrers: decoded from the stock bytes at the corresponding site
// for absolute and pc-relative relocations. -1 unless exactly one candidate gets votes.
func (d *differ) voteByReferences(hi int32, cands []int32) int32 {
	H, S := d.H, d.S
	if d.incoming == nil {
		d.incoming = map[int32][]int32{}
		for k := range H.relocs {
			r := &H.relocs[k]
			if len(H.byName[H.tnames[r.tname]]) < 2 {
				continue
			}
			if t := d.hlinkTarget(r); t >= 0 {
				d.incoming[t] = append(d.incoming[t], int32(k))
			}
		}
	}
	votes := map[int32]int{}
	for _, k := range d.incoming[hi] {
		r := &H.relocs[k]
		src := H.containing(r.site)
		if src < 0 || H.syms[src].match < 0 {
			continue
		}
		s := &S.syms[H.syms[src].match]
		ssite := s.addr + (r.site - H.syms[src].addr)
		if !S.img.inFile(ssite, uint64(r.siz)) {
			continue
		}
		v := readSized(S.img.view(ssite, uint64(r.siz)))
		var ta uint64
		switch t := objabi.RelocType(r.typ); {
		case t == objabi.R_ADDR && r.siz == 8:
			ta = uint64(int64(v) - r.add)
		case (t == objabi.R_CALL || t == objabi.R_PCREL) && r.siz == 4:
			ta = uint64(int64(int32(uint32(v))) + int64(ssite) + 4 - r.add)
		default:
			continue
		}
		for _, c := range cands {
			if S.syms[c].addr == ta {
				votes[c]++
			}
		}
	}
	if len(votes) != 1 {
		return -1
	}
	for c := range votes {
		return c
	}
	return -1
}

// relocsAgree: the stock bytes of s at hlink's relocation sites of h are the relocations evaluated with the
// stock addresses (unknown: some target is not matched yet, and was not counted against).
func (d *differ) relocsAgree(hi, si int32) (ok, unknown bool) {
	h, s := &d.H.syms[hi], &d.S.syms[si]
	if !d.S.img.inFile(s.addr, s.size) {
		return true, false
	}
	for _, r := range d.H.relocsIn(h.addr, h.size) {
		o := r.site - h.addr
		if o+uint64(r.siz) > h.size {
			return false, false
		}
		hti := d.hlinkTarget(&r)
		if hti >= 0 && d.H.syms[hti].match < 0 && d.H.syms[hti].class != cBoundary {
			unknown = true
			continue
		}
		want, mode, _, _, _ := d.stockExpect(&r, hti, s.addr+o)
		sv := readSized(d.S.img.view(s.addr+o, uint64(r.siz)))
		switch mode {
		case vBad:
			unknown = true
		case vKeep:
			if sv != readSized(d.H.img.view(r.site, uint64(r.siz))) {
				return false, false
			}
		default:
			if sv != want {
				return false, false
			}
		}
	}
	return true, unknown
}

// sameMasked: the two symbols' bytes are equal outside hlink's relocation sites.
func (d *differ) sameMasked(hi, si int32) bool {
	h, s := &d.H.syms[hi], &d.S.syms[si]
	if h.size != s.size {
		return false
	}
	if !d.H.img.inFile(h.addr, h.size) || !d.S.img.inFile(s.addr, s.size) {
		return true
	}
	hb, sb := d.H.img.view(h.addr, h.size), d.S.img.view(s.addr, s.size)
	off := uint64(0)
	for _, r := range d.H.relocsIn(h.addr, h.size) {
		o := r.site - h.addr
		if o > off && !bytes.Equal(hb[off:o], sb[off:o]) {
			return false
		}
		off = max(off, o+uint64(r.siz))
	}
	off = min(off, h.size)
	return bytes.Equal(hb[off:], sb[off:])
}

// ---- 2. sizes and alignment

func defaultAlign(size uint64) uint64 {
	a := uint64(32) // amd64 Maxalign
	for a > size && a > 1 {
		a >>= 1
	}
	return a
}

func (d *differ) checkSizes() {
	H, S := d.H, d.S
	st := rep.stat("sizes+alignment")
	// hlink: no two symbols overlap
	for k := 1; k < len(H.byAddr); k++ {
		a, b := &H.syms[H.byAddr[k-1]], &H.syms[H.byAddr[k]]
		st.compared++
		if a.addr+a.size > b.addr {
			st.different++
			rep.diff("sizes+alignment", "hlink: symbols overlap", "%s @%#x+%d and %s @%#x", a.name, a.addr, a.size, b.name, b.addr)
		} else {
			st.equal++
		}
	}
	for i := range H.syms {
		h := &H.syms[i]
		if h.match < 0 {
			continue
		}
		s := &S.syms[h.match]
		switch h.class {
		case cBoundary, cLinker, cPclntab:
			continue
		}
		st.compared++
		bad := false
		if h.size != s.size {
			bad = true
			rep.diff("sizes+alignment", "size", "%s: hlink %d, stock %d", h.name, h.size, s.size)
		}
		req := uint64(s.align)
		if req == 0 {
			req = defaultAlign(s.size)
		}
		if h.class == cText {
			req = max(req, 32) // funcAlign
		}
		if s.addr%req != 0 {
			bad = true
			rep.diff("sizes+alignment", "stock address not aligned as the rule says (secdiff's rule is wrong)", "%s @%#x: align %d", s.name, s.addr, req)
		}
		if h.addr%req != 0 {
			bad = true
			rep.diff("sizes+alignment", "alignment", "%s (%s): hlink @%#x is not %d-aligned (stock @%#x)", h.name, className[h.class], h.addr, req, s.addr)
		}
		if bad {
			st.different++
		} else {
			st.equal++
		}
	}
}

// ---- 3. content and relocations

func hasContent(c uint8) bool {
	switch c {
	case cText, cRodata, cTypes, cItabs, cFuncdata, cNoptrdata, cData:
		return true
	}
	return false
}

func (d *differ) checkContent() {
	H, S := d.H, d.S
	st := rep.stat("content")
	rst := rep.stat("relocations")
	covered := 0 // relocations inside symbols looked at
	for i := range H.syms {
		h := &H.syms[i]
		if !hasContent(h.class) || h.size == 0 {
			continue
		}
		relocs := H.relocsIn(h.addr, h.size)
		if h.match < 0 {
			rst.unmatched += len(relocs)
			covered += len(relocs)
			continue
		}
		s := &S.syms[h.match]
		if !hasContent(s.class) || s.size != h.size {
			rst.unmatched += len(relocs)
			covered += len(relocs)
			continue
		}
		if !H.img.inFile(h.addr, h.size) || !S.img.inFile(s.addr, s.size) {
			rep.diff("content", "symbol with content not in the file", "%s: hlink @%#x, stock @%#x", h.name, h.addr, s.addr)
			st.different++
			continue
		}
		st.compared++
		hb, sb := H.img.view(h.addr, h.size), S.img.view(s.addr, s.size)
		ok := true
		off := uint64(0)
		cmpRange := func(a, b uint64) {
			if a >= b || !ok {
				return
			}
			if !bytes.Equal(hb[a:b], sb[a:b]) {
				k := a
				for hb[k] == sb[k] {
					k++
				}
				ok = false
				rep.diff("content", "bytes differ outside relocation sites: "+className[h.class], "%s+%#x (of %d): hlink % x, stock % x",
					h.name, k, h.size, hb[k:min(k+8, h.size)], sb[k:min(k+8, h.size)])
			}
		}
		for ri := range relocs {
			r := &relocs[ri]
			covered++
			o := r.site - h.addr
			if o+uint64(r.siz) > h.size {
				rst.different++
				rep.diff("relocations", "site beyond the end of its symbol", "%s+%#x siz %d (size %d)", h.name, o, r.siz, h.size)
				continue
			}
			cmpRange(off, o)
			off = max(off, o+uint64(r.siz))
			rst.compared++
			if d.checkReloc(h, s, r, hb[o:o+uint64(r.siz)], sb[o:o+uint64(r.siz)]) {
				rst.equal++
			} else {
				rst.different++
			}
		}
		cmpRange(off, h.size)
		if ok {
			st.equal++
		} else {
			st.different++
		}
	}
	if n := len(H.relocs) - covered; n > 0 {
		// relocations outside every symbol with content: find them
		k := 0
		for i := range H.relocs {
			r := &H.relocs[i]
			hi := H.containing(r.site)
			if hi >= 0 && hasContent(H.syms[hi].class) {
				continue
			}
			k++
			rst.different++
			rep.diff("relocations", "site outside every symbol with content", "R at %#x (%s) to %s", r.site, d.desc(H, r.site), H.tnames[r.tname])
		}
		_ = k
	}
}

func readSized(b []byte) uint64 {
	switch len(b) {
	case 1:
		return uint64(b[0])
	case 2:
		return uint64(le.Uint16(b))
	case 4:
		return uint64(le.Uint32(b))
	case 8:
		return le.Uint64(b)
	}
	return 0
}

func truncate(v int64, siz uint8) uint64 {
	if siz >= 8 {
		return uint64(v)
	}
	return uint64(v) & (1<<(8*uint(siz)) - 1)
}

var relocNames = map[uint16]string{}

func rname(t uint16) string {
	if n, ok := relocNames[t]; ok {
		return n
	}
	n := objabi.RelocType(t).String()
	relocNames[t] = n
	return n
}

// hlinkSectBase: the base of the section an R_ADDROFF target is in, by hlink's boundaries (fclink sectBase).
func (d *differ) hlinkSectBase(a uint64) uint64 {
	b := func(n string) uint64 { v, _ := d.H.addrOf(n); return v }
	switch {
	case a < b("runtime.rodata"):
		return b("runtime.text")
	case a < b("runtime.pclntab"):
		return b("runtime.rodata")
	case a < b("runtime.types"):
		return b("runtime.pclntab")
	case a < b("runtime.noptrdata"):
		return b("runtime.types")
	case a < b("runtime.data"):
		return b("runtime.noptrdata")
	case a < b("runtime.bss"):
		return b("runtime.data")
	case a < b("runtime.noptrbss"):
		return b("runtime.bss")
	}
	return b("runtime.noptrbss")
}

// stockSectBase: the stock linker's rule (data.go relocsym, R_ADDROFF): the vaddr of the target's section,
// the first text section for text.
func (d *differ) stockSectBase(si int32) (uint64, bool) {
	name := d.S.syms[si].sect
	if name == ".text" {
		name = ".text"
	}
	if sc := d.S.sectionByName(name); sc != nil {
		return sc.vaddr, true
	}
	return 0, false
}

const (
	vValue = iota
	vKeep  // the relocation leaves the object's bytes alone (a weak R_ADDROFF to an unreachable target)
	vBad
)

// formula evaluates relocation r at site with the given target (addr, size, section base); unreach says the
// target is a weak reference to a symbol not in the output, unreachableMethod/mapinitnoop the substitutes.
func formula(r *reloc, site, taddr, tsize, tbase uint64, unreach bool, unreachableMethod, mapinitnoop uint64) (uint64, int, string) {
	add := r.add
	switch objabi.RelocType(r.typ) {
	case objabi.R_ADDR:
		if unreach {
			taddr = unreachableMethod
		}
		return truncate(int64(taddr)+add, r.siz), vValue, ""
	case objabi.R_METHODOFF:
		if unreach {
			return truncate(-1, r.siz), vValue, ""
		}
		return truncate(int64(taddr-tbase)+add, r.siz), vValue, ""
	case objabi.R_ADDROFF:
		if unreach {
			return 0, vKeep, ""
		}
		return truncate(int64(taddr-tbase)+add, r.siz), vValue, ""
	case objabi.R_CALL, objabi.R_PCREL:
		if unreach {
			if objabi.RelocType(r.typ) != objabi.R_CALL {
				return 0, vBad, "pc-relative reference to an unreachable symbol"
			}
			taddr = mapinitnoop
		}
		return truncate(int64(taddr)+add-int64(site+uint64(r.siz)), r.siz), vValue, ""
	case objabi.R_TLS_LE:
		return truncate(-8+add, r.siz), vValue, "" // ELF amd64: Tlsoffset = -8
	case objabi.R_CONST:
		return truncate(add, r.siz), vValue, ""
	case objabi.R_SIZE:
		return truncate(int64(tsize)+add, r.siz), vValue, ""
	}
	return 0, vBad, "relocation type not handled by secdiff"
}

// hlinkTarget: the hlink symbol a relocation names (at its target address, by its name); -1 if none.
func (d *differ) hlinkTarget(r *reloc) int32 {
	if r.taddr == 0 {
		return -1
	}
	tname := d.H.tnames[r.tname]
	for _, i := range d.H.at(r.taddr) {
		if d.H.syms[i].name == tname {
			return i
		}
	}
	return -1
}

// stockExpect evaluates relocation r with the stock addresses, for a site at ssite in the stock binary.
// It returns the value, the mode, the stock target, and on failure the kind of problem and a detail.
func (d *differ) stockExpect(r *reloc, hti int32, ssite uint64) (want uint64, mode int, sti int32, kind, detail string) {
	H, S := d.H, d.S
	tname := H.tnames[r.tname]
	sti = -1
	sunreach := false
	if tname != "" {
		if hti >= 0 {
			if m := H.syms[hti].match; m >= 0 {
				sti = m
			} else if i := S.named(tname); i >= 0 && H.syms[hti].class == cBoundary {
				sti = i
			}
			if sti < 0 {
				return 0, vBad, -1, "target has no stock counterpart", fmt.Sprintf("(hlink @%#x)", r.taddr)
			}
		} else {
			if i := S.named(tname); i >= 0 {
				return 0, vBad, i, "weak target pruned by hlink but kept by the stock linker", fmt.Sprintf("(stock %s @%#x)", tname, S.syms[i].addr)
			}
			if len(S.byName[tname]) > 0 {
				return 0, vBad, -1, "weak target pruned by hlink but kept by the stock linker", "(stock has several)"
			}
			sunreach = true
		}
	}
	var staddr, stsize, stbase uint64
	if sti >= 0 {
		staddr, stsize = S.syms[sti].addr, S.syms[sti].size
		if t := objabi.RelocType(r.typ); t == objabi.R_ADDROFF || t == objabi.R_METHODOFF {
			b, ok := d.stockSectBase(sti)
			if !ok {
				return 0, vBad, sti, "stock: target's section unknown", "(section " + S.syms[sti].sect + ")"
			}
			stbase = b
		}
	}
	sum, _ := S.addrOf("runtime.unreachableMethod")
	smi, _ := S.addrOf("runtime.mapinitnoop")
	want, mode, why := formula(r, ssite, staddr, stsize, stbase, sunreach, sum, smi)
	if mode == vBad {
		return 0, vBad, sti, "stock: " + why, ""
	}
	return want, mode, sti, "", ""
}

// checkReloc checks one relocation site of matched symbols h (hlink) and s (stock): hlink's bytes against
// its own map, the stock bytes against the same relocation evaluated with the stock addresses.
func (d *differ) checkReloc(h, s *sym, r *reloc, hb, sb []byte) bool {
	H, S := d.H, d.S
	tname := H.tnames[r.tname]
	hv, sv := readSized(hb), readSized(sb)
	site := r.site
	ssite := s.addr + (site - h.addr)
	where := fmt.Sprintf("%s+%#x %s/%d -> %s%+d", h.name, site-h.addr, rname(r.typ), r.siz, tname, r.add)

	// ---- hlink, against its own map
	unreach := r.taddr == 0 && tname != ""
	var htsize uint64
	hti := d.hlinkTarget(r)
	if r.taddr != 0 {
		if hti < 0 {
			rep.diff("relocations", "hlink: target not in hlink's map at the address the relocation names", "%s: no %s at %#x", where, tname, r.taddr)
			return false
		}
		htsize = H.syms[hti].size
	}
	hum, _ := H.addrOf("runtime.unreachableMethod")
	hmi, _ := H.addrOf("runtime.mapinitnoop")
	want, mode, why := formula(r, site, r.taddr, htsize, d.hlinkSectBase(r.taddr), unreach, hum, hmi)
	if mode == vBad {
		rep.diff("relocations", "hlink: "+why, "%s", where)
		return false
	}
	if mode == vValue && hv != want {
		rep.diff("relocations", "hlink: value is not the formula with hlink's own addresses", "%s: hlink has %#x, its map says %#x", where, hv, want)
		return false
	}

	// ---- stock, with the stock addresses of the same target
	swant, smode, sti, kind, detail := d.stockExpect(r, hti, ssite)
	if smode == vBad {
		rep.diff("relocations", kind, "%s %s", where, detail)
		return false
	}
	if smode == vKeep || mode == vKeep {
		if hv != sv {
			rep.diff("relocations", "weak R_ADDROFF to an unreachable target: bytes differ", "%s: hlink %#x, stock %#x", where, hv, sv)
			return false
		}
		return true
	}
	if sv == swant {
		return true
	}
	var staddr uint64
	if sti >= 0 {
		staddr = S.syms[sti].addr
	}
	// an identical duplicate of the target (same name, size and content) will do
	if sti >= 0 && S.syms[sti].eq != 0 {
		delta := int64(sv) - int64(swant)
		alt := uint64(int64(staddr) + delta)
		for _, i := range S.at(alt) {
			if S.syms[i].eq == S.syms[sti].eq {
				rep.allow("D1", "%s: stock points at %s", where, d.desc(S, alt))
				return true
			}
		}
	}
	got := "?"
	switch objabi.RelocType(r.typ) {
	case objabi.R_ADDR:
		got = d.desc(S, sv-uint64(r.add))
	case objabi.R_CALL, objabi.R_PCREL:
		got = d.desc(S, uint64(int64(int32(sv))+int64(ssite)+int64(r.siz)-r.add))
	}
	rep.diff("relocations", "stock value is not the formula with the stock addresses", "%s: stock has %#x (%s), formula with %s gives %#x", where, sv, got, d.desc(S, staddr), swant)
	return false
}

// predict: the bytes the stock binary should have for hlink symbol h, from hlink's bytes and relocations
// evaluated with the stock addresses; nil if that needs the stock address of h itself (pc-relative) or of
// a target not matched yet.
func (d *differ) predict(hi int32) []byte {
	h := &d.H.syms[hi]
	if !d.H.img.inFile(h.addr, h.size) {
		return nil
	}
	p := d.H.img.bytes(h.addr, h.size)
	for _, r := range d.H.relocsIn(h.addr, h.size) {
		switch objabi.RelocType(r.typ) {
		case objabi.R_CALL, objabi.R_PCREL:
			return nil
		}
		o := r.site - h.addr
		if o+uint64(r.siz) > h.size {
			return nil
		}
		v, mode, _, _, _ := d.stockExpect(&r, d.hlinkTarget(&r), 0)
		switch mode {
		case vBad:
			return nil
		case vKeep:
			continue
		}
		switch r.siz {
		case 1:
			p[o] = byte(v)
		case 2:
			le.PutUint16(p[o:], uint16(v))
		case 4:
			le.PutUint32(p[o:], uint32(v))
		case 8:
			le.PutUint64(p[o:], v)
		}
	}
	return p
}

// matchByContent: symbols left unmatched by name that are the same content-addressed symbol kept under
// another name (rule H1): the stock bytes are exactly what hlink's bytes predict.
func (d *differ) matchByContent() {
	H, S := d.H, d.S
	type key struct {
		class uint8
		size  uint64
		h     string
	}
	for pass := 0; pass < 8; pass++ {
		idx := map[key][]int32{}
		for i := range S.syms {
			s := &S.syms[i]
			if s.match >= 0 || !hasContent(s.class) || s.size == 0 || s.class == cText || !S.img.inFile(s.addr, s.size) {
				continue
			}
			k := key{s.class, s.size, string(S.img.view(s.addr, s.size))}
			idx[k] = append(idx[k], int32(i))
		}
		n := 0
		for i := range H.syms {
			h := &H.syms[i]
			if h.match >= 0 || !hasContent(h.class) || h.size == 0 || h.class == cText {
				continue
			}
			p := d.predict(int32(i))
			if p == nil {
				continue
			}
			k := key{h.class, h.size, string(p)}
			if l := idx[k]; len(l) > 0 {
				d.pair(int32(i), l[0])
				idx[k] = l[1:]
				h.byContent = true
				n++
			}
		}
		if n == 0 {
			break
		}
	}
}

// ---- rules for symbols in one map only

func (d *differ) allowOnlyHlink(h *sym) bool {
	switch {
	case h.class == cBoundary && (boundaryNames[h.name] || pclntabNames[h.name] || h.name == "runtime.pclntab"):
		rep.allow("B1", "%s", h.name)
		return true
	}
	return false
}

// referenced: some hlink relocation targets this name.
func (d *differ) referenced(name string) bool {
	if d.refd == nil {
		d.refd = map[string]bool{}
		for i := range d.H.relocs {
			d.refd[d.H.tnames[d.H.relocs[i].tname]] = true
		}
	}
	return d.refd[name]
}

func (d *differ) allowOnlyStock(s *sym) bool {
	switch {
	case s.class == cCarrier:
		rep.allow("C1", "%s (%s, %d bytes)", s.name, s.sect, s.size)
		return true
	case s.name == "" && s.sect == ".gopclntab":
		rep.allow("I1", "inline tree @%#x, %d bytes", s.addr, s.size)
		return true
	case s.class == cBoundary || s.class == cPclntab:
		rep.allow("B1", "%s", s.name)
		return true
	case s.name == "go:buildinfo.ref":
		rep.allow("L1", "%s", s.name)
		return true
	case s.name == "go:fipsinfo" && !d.referenced("go:fipsinfo"):
		rep.allow("L2", "%s", s.name)
		return true
	}
	return false
}

func defineRules() {
	rep.defRule("C1", "stock carrier symbols (type:*, go:string.*, go:funcdesc, runtime.gcbits.*, runtime.pclntab) have no hlink counterpart",
		"cmd/link symtab.go groupSym/setCarrierSym and pcln.go make container symbols that only span the symbols they carry, for the symbol table; hlink writes no symbol table, the carried symbols are compared one by one")
	rep.defRule("I1", "stock inline-tree symbols (unnamed, in go:func.*) are not symbols in hlink",
		"cmd/link pcln.go genInlTreeSym makes one unnamed symbol per function with inlining; hlink writes the same 16-byte entries straight into go:func.*: compared entry by entry in the pclntab check")
	rep.defRule("B1", "boundary symbols and pclntab pieces one linker names and the other does not",
		"both linkers define the section boundaries; each only names the ones it needs (fclink defineSyms names a fixed set, cmd/link xdefines others); their values are checked through moduledata")
	rep.defRule("L1", "the stock linker's go:buildinfo.ref (a pointer to go:buildinfo in rodata) has no hlink counterpart",
		"cmd/link data.go buildinfo adds it only so that external linking with --gc-sections keeps go:buildinfo; nothing reads it and hlink links internally only")
	rep.defRule("L2", "the stock linker writes go:fipsinfo when nothing refers to it",
		"cmd/link fips140.go loadfips creates go:fipsinfo whenever FIPS is enabled; hlink (fclink makeSpecials) only when crypto/internal/fips140/check refers to it, the only reader; allowed only when no hlink relocation targets it")
	rep.defRule("H1", "a content-addressed symbol is kept under different names: matched by content",
		"content-addressed symbols (goobj hashed64/hashed definitions: strings, stack-object and GC-bit tables, ..stmp_N, gcbits) are deduplicated by content hash across names (loader.go addSym); which copy, so which name, survives depends on load order (stock: object load order; fclink: importcfg order), and names are not in the output; the stock bytes must be exactly hlink's bytes with the relocations evaluated at the stock addresses")
	rep.defRule("F1", "the stock functab lists the FIPS text markers go:textfipsstart and go:textfipsend as functions",
		"cmd/link fips140.go makes them 1-byte STEXTFIPSSTART/END text symbols, so they are in Textp and get a _func (pcln.go makePclntab); they hold no code and nothing calls them; hlink defines them as boundaries")
	rep.defRule("F2", "hlink's file tables list every file of an object with a live function; the stock ones only the files live functions use",
		"cmd/link pcln.go generateFilenameTabs walks the pcfile tables of the live functions (walkFilenames) and trims each CU's cutab to its largest used index; the runtime only reaches files through pcfile, which is compared decoded to names; the stock file names must all be in hlink's table")
	rep.defRule("P1", "the stock pctab is longer by its rounding to 8 bytes",
		"cmd/link pcln.go addGeneratedSym rounds every pclntab piece's size up to the pointer size; allowed only when both binaries' functions reference exactly the same number of pctab bytes and each side has fewer than 9 unreferenced bytes (offset 0 is reserved in both)")
	rep.defRule("D1", "a relocation resolves to a different one of several identical duplicates (same name, size and content)",
		"static symbols with the same name and identical content (e.g. two packages' identical ..stmp_N) are interchangeable; which one a reference picks is not observable")
}
