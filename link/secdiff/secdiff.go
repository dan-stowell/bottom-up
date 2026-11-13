// secdiff: a symbolic diff of two linkers' outputs for the same objects. The linker under test (hlink, or
// fclink with -dumpmap) and the stock linker (cmd/link with stocklink.patch) place the same symbols at
// different addresses, so the two binaries cannot be compared byte by byte; but given where every symbol is
// in each (the maps) and where every relocation of the linker under test is and what it targets, the stock
// binary's bytes are predictable from the other's: outside relocation sites a symbol's bytes are the same;
// at each site the stock value is the relocation's formula evaluated with the stock addresses. Anything
// else is a bug in one of the linkers, or a deliberate difference listed (with its reason) in the rules
// table the tool prints.
//
//	secdiff [-n N] [-v] hlink.map hlink.bin stock.map stock.bin
//
// Map formats (the name is the rest of the line; names may contain spaces):
//
//	hlink: S <addr-hex> <size-dec> <bucket> <name>
//	       R <site-hex> <siz-dec> <objabi type-dec, weak bit cleared> <target-addr-hex> <addend-dec> <target-name>
//	stock: X <vaddr-hex> <length-dec> <section>
//	       S <addr-hex> <size-dec> <section> <name>
//	       L <addr-hex> <align-dec> <name>
//
// Checks: symbol sets, sizes and alignment, content and relocations, pclntab, moduledata and the
// linker-made symbols, the GC bitmaps of data and bss, the order the runtime needs in the types section.
// Exit status 0 only if no difference is found beyond the rules.
package main

import (
	"bufio"
	"bytes"
	"debug/elf"
	"flag"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
)

var (
	flagN       = flag.Int("n", 10, "show the first `N` differences of each kind")
	flagVerbose = flag.Bool("v", false, "also show examples of differences covered by rules")
	flagLocate  = flag.String("locate", "", "print the address of `pcsp:FUNC`'s pcsp table in the first binary and exit (for selftest.sh)")
)

// ---- classes of symbols

const (
	cNone = iota
	cText
	cRodata
	cTypes
	cItabs
	cFuncdata
	cPclntab // pclntab pieces the linker makes
	cNoptrdata
	cData
	cBss
	cNoptrbss
	cLinker   // content the linker writes (moduledata, init tasks, string variables, build info, ...)
	cBoundary // size-0 symbols naming places in the image
	cCarrier  // stock: symbols that only carry others (type:*, go:string.*, ...)
	nClass
)

var className = [nClass]string{"none", "text", "rodata", "types", "itabs", "funcdata", "pclntab", "noptrdata",
	"data", "bss", "noptrbss", "linker", "boundary", "carrier"}

// boundary symbols: both linkers define these (the stock linker with xdefine, hlink in defineSyms)
var boundaryNames = map[string]bool{}

// pclntab pieces: generator symbols of the stock linker, boundaries in hlink
var pclntabNames = map[string]bool{}

// symbols whose content the linker writes
var linkerNames = map[string]bool{
	"runtime.firstmoduledata": true, "runtime.lastmoduledatap": true, "go:buildinfo": true, "go:fipsinfo": true,
	"go:main.inittasks": true, "go:runtime.inittasks": true, "runtime.runtime_inittasks": true,
	"runtime.textsectionmap": true, "go:buildinfo.ref": true,
}

// stock carrier symbols (symtab.go: groupSym, setCarrierSym; pcln.go: runtime.pclntab)
var carrierNames = map[string]bool{
	"type:*": true, "go:string.*": true, "go:funcdesc": true, "runtime.gcbits.*": true, "runtime.pclntab": true,
}

func init() {
	for _, n := range strings.Fields(`runtime.text runtime.etext runtime.rodata runtime.erodata runtime.types
		runtime.etypes runtime.noptrdata runtime.enoptrdata runtime.data runtime.edata runtime.bss runtime.ebss
		runtime.noptrbss runtime.enoptrbss runtime.covctrs runtime.ecovctrs runtime.end runtime.egcdata
		runtime.egcbss runtime.gcmask.* go:textfipsstart go:textfipsend go:rodatafipsstart go:rodatafipsend
		go:noptrdatafipsstart go:noptrdatafipsend go:datafipsstart go:datafipsend runtime.gcdata runtime.gcbss`) {
		boundaryNames[n] = true
	}
	for _, n := range strings.Fields(`runtime.pcheader runtime.funcnametab runtime.cutab runtime.filetab
		runtime.pctab runtime.functab runtime.findfunctab go:func.* runtime.epclntab`) {
		pclntabNames[n] = true
	}
}

func hlinkClass(bucket, name string) uint8 {
	switch bucket {
	case "text", "textfips":
		return cText
	case "rodata", "rodatafips":
		return cRodata
	case "gofunc":
		return cFuncdata
	case "typelink", "type":
		return cTypes
	case "itab":
		return cItabs
	case "noptrdata", "noptrdatafips":
		return cNoptrdata
	case "data", "datafips":
		return cData
	case "bss":
		return cBss
	case "noptrbss", "cov":
		return cNoptrbss
	case "special":
		return cLinker
	case "boundary":
		return cBoundary
	}
	return cNone
}

func stockClass(sect, name string) uint8 {
	switch {
	case carrierNames[name]:
		return cCarrier
	case boundaryNames[name]:
		return cBoundary
	case pclntabNames[name]:
		return cPclntab
	case linkerNames[name]:
		return cLinker
	}
	switch sect {
	case ".text":
		return cText
	case ".rodata", ".go.func":
		return cRodata
	case ".go.type":
		if strings.HasPrefix(name, "go:itab.") {
			return cItabs
		}
		return cTypes
	case ".gopclntab":
		return cFuncdata
	case ".noptrdata":
		return cNoptrdata
	case ".data":
		return cData
	case ".bss":
		return cBss
	case ".noptrbss":
		return cNoptrbss
	case ".go.buildinfo", ".go.fipsinfo", ".go.module":
		return cLinker
	}
	return cNone
}

// ---- maps

type sym struct {
	addr, size uint64
	name       string
	sect       string // hlink: bucket; stock: section
	class      uint8
	align      uint32 // stock: the alignment, when not the size-derived default (L lines)
	match      int32  // index of the matched symbol in the other map, -1
	eq         int32  // identical duplicates share an id (0: none)
	byContent  bool   // hlink: matched by content, not by name (rule H1)
}

type reloc struct {
	site, taddr uint64
	add         int64
	tname       int32 // index in lmap.tnames
	typ         uint16
	siz         uint8
}

type section struct {
	vaddr, length uint64
	name          string
}

type lmap struct {
	path   string
	stock  bool
	syms   []sym            // by address (stable: file order among equal addresses)
	byName map[string][]int32
	byAddr []int32 // syms with size > 0 and content in the image, by address (hlink: for containment)
	sects  []section
	relocs []reloc // by site
	tnames []string
	img    *image
}

func readMap(path string, stock bool) *lmap {
	f, err := os.Open(path)
	if err != nil {
		fatalf("%v", err)
	}
	defer f.Close()
	m := &lmap{path: path, stock: stock, byName: map[string][]int32{}}
	intern := map[string]string{}
	tnameIdx := map[string]int32{}
	str := func(b []byte) string {
		if s, ok := intern[string(b)]; ok {
			return s
		}
		s := string(b)
		intern[s] = s
		return s
	}
	type align struct {
		addr  uint64
		align uint32
		name  string
	}
	var aligns []align
	r := bufio.NewReaderSize(f, 1<<20)
	lineNo := 0
	field := func(line []byte) ([]byte, []byte) {
		i := bytes.IndexByte(line, ' ')
		if i < 0 {
			return line, nil
		}
		return line[:i], line[i+1:]
	}
	hex := func(b []byte) uint64 {
		v, err := strconv.ParseUint(string(b), 16, 64)
		if err != nil {
			fatalf("%s:%d: bad hex %q", path, lineNo, b)
		}
		return v
	}
	dec := func(b []byte) int64 {
		v, err := strconv.ParseInt(string(b), 10, 64)
		if err != nil {
			fatalf("%s:%d: bad number %q", path, lineNo, b)
		}
		return v
	}
	for {
		line, err := r.ReadSlice('\n')
		if err == bufio.ErrBufferFull {
			fatalf("%s:%d: line too long", path, lineNo)
		}
		if len(line) == 0 && err != nil {
			break
		}
		lineNo++
		line = bytes.TrimSuffix(line, []byte("\n"))
		if len(line) < 2 {
			continue
		}
		kind := line[0]
		rest := line[2:]
		switch kind {
		case 'S':
			a, rest := field(rest)
			sz, rest := field(rest)
			sect, name := field(rest)
			s := sym{addr: hex(a), size: uint64(dec(sz)), sect: str(sect), name: str(name), match: -1}
			if stock {
				s.class = stockClass(s.sect, s.name)
			} else {
				s.class = hlinkClass(s.sect, s.name)
			}
			m.syms = append(m.syms, s)
		case 'R':
			site, rest := field(rest)
			siz, rest := field(rest)
			typ, rest := field(rest)
			ta, rest := field(rest)
			add, name := field(rest)
			ti, ok := tnameIdx[string(name)]
			if !ok {
				ti = int32(len(m.tnames))
				m.tnames = append(m.tnames, str(name))
				tnameIdx[m.tnames[ti]] = ti
			}
			m.relocs = append(m.relocs, reloc{site: hex(site), siz: uint8(dec(siz)), typ: uint16(dec(typ)), taddr: hex(ta), add: dec(add), tname: ti})
		case 'X':
			a, rest := field(rest)
			ln, name := field(rest)
			m.sects = append(m.sects, section{hex(a), uint64(dec(ln)), string(name)})
		case 'L':
			a, rest := field(rest)
			al, name := field(rest)
			aligns = append(aligns, align{hex(a), uint32(dec(al)), str(name)})
		default:
			fatalf("%s:%d: unknown line %q", path, lineNo, line)
		}
		if err != nil {
			break
		}
	}
	sort.SliceStable(m.syms, func(i, j int) bool { return m.syms[i].addr < m.syms[j].addr })
	sort.Slice(m.relocs, func(i, j int) bool { return m.relocs[i].site < m.relocs[j].site })
	for i := range m.syms {
		s := &m.syms[i]
		m.byName[s.name] = append(m.byName[s.name], int32(i))
	}
	for _, a := range aligns {
		for _, i := range m.byName[a.name] {
			if m.syms[i].addr == a.addr {
				m.syms[i].align = a.align
			}
		}
	}
	for i := range m.syms {
		s := &m.syms[i]
		if s.size > 0 && s.class != cCarrier && s.class != cBoundary {
			m.byAddr = append(m.byAddr, int32(i))
		}
	}
	return m
}

// containing returns the index of the symbol (with size > 0) whose bytes contain address a, or -1.
func (m *lmap) containing(a uint64) int32 {
	k := sort.Search(len(m.byAddr), func(k int) bool { return m.syms[m.byAddr[k]].addr > a }) - 1
	for ; k >= 0; k-- {
		s := &m.syms[m.byAddr[k]]
		if a < s.addr+s.size {
			return m.byAddr[k]
		}
		if s.addr+s.size+(1<<16) < a { // (stock: symbols do not nest beyond carriers, which are not in byAddr)
			break
		}
	}
	return -1
}

// at returns the indices of the symbols at address a.
func (m *lmap) at(a uint64) []int32 {
	lo := sort.Search(len(m.syms), func(k int) bool { return m.syms[k].addr >= a })
	var r []int32
	for k := lo; k < len(m.syms) && m.syms[k].addr == a; k++ {
		r = append(r, int32(k))
	}
	return r
}

// named returns the one symbol with this name that is not a carrier, or -1 (none, or several).
func (m *lmap) named(name string) int32 {
	r := int32(-1)
	for _, i := range m.byName[name] {
		if m.syms[i].class == cCarrier {
			continue
		}
		if r >= 0 {
			return -1
		}
		r = i
	}
	return r
}

func (m *lmap) addrOf(name string) (uint64, bool) {
	if i := m.named(name); i >= 0 {
		return m.syms[i].addr, true
	}
	return 0, false
}

// relocsIn returns the relocations whose sites are in [a, a+n).
func (m *lmap) relocsIn(a, n uint64) []reloc {
	lo := sort.Search(len(m.relocs), func(k int) bool { return m.relocs[k].site >= a })
	hi := lo
	for hi < len(m.relocs) && m.relocs[hi].site < a+n {
		hi++
	}
	return m.relocs[lo:hi]
}

// sectionOf (stock): the section a symbol's section name stands for (the first of that name).
func (m *lmap) sectionByName(name string) *section {
	for i := range m.sects {
		if m.sects[i].name == name {
			return &m.sects[i]
		}
	}
	return nil
}

// ---- the binaries

type image struct {
	path  string
	data  []byte
	progs []*elf.Prog
	f     *elf.File
}

func readImage(path string) *image {
	data, err := os.ReadFile(path)
	if err != nil {
		fatalf("%v", err)
	}
	f, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		fatalf("%s: %v", path, err)
	}
	im := &image{path: path, data: data, f: f}
	for _, p := range f.Progs {
		if p.Type == elf.PT_LOAD {
			im.progs = append(im.progs, p)
		}
	}
	return im
}

// bytes returns the n bytes at address a (zeros past a segment's file size), or nil if a is not mapped.
func (im *image) bytes(a, n uint64) []byte {
	for _, p := range im.progs {
		if a >= p.Vaddr && a+n <= p.Vaddr+p.Memsz {
			out := make([]byte, n)
			if a < p.Vaddr+p.Filesz {
				end := min(a+n, p.Vaddr+p.Filesz)
				copy(out, im.data[p.Off+(a-p.Vaddr):p.Off+(end-p.Vaddr)])
			}
			return out
		}
	}
	return nil
}

// view returns the bytes at [a, a+n) without copying when they are in the file, else as bytes does.
func (im *image) view(a, n uint64) []byte {
	for _, p := range im.progs {
		if a >= p.Vaddr && a+n <= p.Vaddr+p.Filesz {
			return im.data[p.Off+(a-p.Vaddr) : p.Off+(a-p.Vaddr)+n]
		}
	}
	return im.bytes(a, n)
}

func (im *image) inFile(a, n uint64) bool {
	for _, p := range im.progs {
		if a >= p.Vaddr && a+n <= p.Vaddr+p.Filesz {
			return true
		}
	}
	return false
}

func (im *image) u64(a uint64) uint64 {
	b := im.view(a, 8)
	if b == nil {
		return 0
	}
	return le.Uint64(b)
}

func (im *image) u32(a uint64) uint32 {
	b := im.view(a, 4)
	if b == nil {
		return 0
	}
	return le.Uint32(b)
}

func (im *image) u8(a uint64) uint8 {
	b := im.view(a, 1)
	if b == nil {
		return 0
	}
	return b[0]
}

// ---- the report

type stat struct {
	name                                 string
	compared, equal, different, unmatched int
	allowed                              int
}

type kind struct {
	check, name string
	count       int
	examples    []string
}

type rule struct {
	id, what, why string
	count         int
	examples      []string
}

type report struct {
	stats []*stat
	kinds []*kind
	kmap  map[string]*kind
	rules []*rule
	rmap  map[string]*rule
}

var rep = &report{kmap: map[string]*kind{}, rmap: map[string]*rule{}}

func (r *report) stat(name string) *stat {
	for _, s := range r.stats {
		if s.name == name {
			return s
		}
	}
	s := &stat{name: name}
	r.stats = append(r.stats, s)
	return s
}

// diff records a difference of the given kind under a check.
func (r *report) diff(check, kname, format string, args ...any) {
	key := check + "\x00" + kname
	k := r.kmap[key]
	if k == nil {
		k = &kind{check: check, name: kname}
		r.kmap[key] = k
		r.kinds = append(r.kinds, k)
	}
	k.count++
	if len(k.examples) < *flagN {
		k.examples = append(k.examples, fmt.Sprintf(format, args...))
	}
}

func (r *report) defRule(id, what, why string) {
	ru := &rule{id: id, what: what, why: why}
	r.rules = append(r.rules, ru)
	r.rmap[id] = ru
}

// allow records something a rule covers.
func (r *report) allow(id, format string, args ...any) {
	ru := r.rmap[id]
	if ru == nil {
		panic("no rule " + id)
	}
	ru.count++
	if len(ru.examples) < *flagN {
		ru.examples = append(ru.examples, fmt.Sprintf(format, args...))
	}
}

func (r *report) print() int {
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	fmt.Fprintf(w, "\n%-24s %10s %10s %10s %10s %10s\n", "check", "compared", "equal", "different", "unmatched", "by rule")
	for _, s := range r.stats {
		fmt.Fprintf(w, "%-24s %10d %10d %10d %10d %10d\n", s.name, s.compared, s.equal, s.different, s.unmatched, s.allowed)
	}
	fmt.Fprintf(w, "\nDeliberate differences (rules), with the number of cases each covered:\n")
	for _, ru := range r.rules {
		fmt.Fprintf(w, "  %-4s %8d  %s\n        why: %s\n", ru.id, ru.count, ru.what, ru.why)
		if *flagVerbose {
			for _, e := range ru.examples {
				fmt.Fprintf(w, "          e.g. %s\n", e)
			}
		}
	}
	total := 0
	for _, k := range r.kinds {
		total += k.count
	}
	if total == 0 {
		fmt.Fprintf(w, "\nNo differences beyond the rules.\n")
		return 0
	}
	fmt.Fprintf(w, "\nDifferences (%d in all):\n", total)
	for _, k := range r.kinds {
		fmt.Fprintf(w, "\n  [%s] %s: %d\n", k.check, k.name, k.count)
		for _, e := range k.examples {
			fmt.Fprintf(w, "      %s\n", e)
		}
	}
	return 1
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "secdiff: "+format+"\n", args...)
	os.Exit(2)
}

func main() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: secdiff [-n N] [-v] hlink.map hlink.bin stock.map stock.bin\n")
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() != 4 {
		flag.Usage()
		os.Exit(2)
	}
	defineRules()
	H := readMap(flag.Arg(0), false)
	H.img = readImage(flag.Arg(1))
	S := readMap(flag.Arg(2), true)
	S.img = readImage(flag.Arg(3))
	d := &differ{H: H, S: S}
	if *flagLocate != "" {
		d.locate(*flagLocate)
		return
	}
	fmt.Printf("hlink: %d symbols, %d relocations (%s)\nstock: %d symbols, %d sections (%s)\n",
		len(H.syms), len(H.relocs), H.path, len(S.syms), len(S.sects), S.path)
	d.matchSymbols()
	d.checkSizes()
	d.checkContent()
	d.checkPclntab()
	d.checkModuledata()
	d.checkLinkerSyms()
	d.checkGC()
	d.checkTypes()
	d.checkELF()
	os.Exit(rep.print())
}
