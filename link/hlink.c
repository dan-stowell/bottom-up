// hlink: a linker for statically linked linux/amd64 Go executables, a C port of simd-lang's fclink. It reads
// the same inputs as `go tool link -s -w -buildmode=exe` (an importcfg.link and the main package's object) and
// writes an ELF executable without DWARF and without a symbol table. CGO_ENABLED=0, internal linking only.
//
//	hlink -o out -importcfg importcfg.link [-buildid id] [-X pkg.name=val]... [-stats] [-j N] [-nodce]
//	      [-dumplive file] [-dumpmap file] [-E entry] main.o
//
// Every phase works on dense symbol ids (object base + index in the object) held in flat arrays:
//
//	load      map every input, find the Go objects in it                                 (parallel per file)
//	resolve   partitioned hash joins by name and by content hash: every symbol slot of
//	          every object resolved to its canonical definition                          (parallel per unit, per partition)
//	inittasks the init order from R_INITORDER                                            (serial, small)
//	deadcode  frontier-parallel flood with the stock linker's method pruning             (parallel per level)
//	layout    sizes per unit, prefix sums over units, addresses; typelinks sorted        (parallel per unit)
//	write     copy + relocate + pclntab entries + GC bitmaps, straight into the mapped
//	          output file                                                                (parallel per unit)
//	finish    moduledata, findfunctab, GC programs, FIPS hash, ELF headers               (serial, small)
//
// A unit is an object or a slice of 8192 slots of a big object, so one huge package does not bound the wall clock.
// The phase files below are included into this one translation unit.
#include "common.h"
#include "main.c"
#include "load.c"
#include "resolve.c"
#include "dead.c"
#include "layout.c"
#include "write.c"
#include "special.c"
