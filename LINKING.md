# LINKING.md — can linking be given a SIMD shape?

*2026-11-10. A design question, answered with a prototype of the core operations
(`explore/link.c`), not with a linker. One thread, this VM, a synthetic job with the
proportions of a large C++ program: 2,000 units, 1,000,000 symbols with 24-88 byte names,
3,000,000 references by name, 12,000,000 relocations, 250 MB of code. It leaves out most of
what a production linker does (archives, COMDAT and weak symbols, section GC, identical code
folding, string merging, debug info, dynamic tables, file I/O, threads), so the numbers say
what the core costs and what it could cost, not how a real linker would compare.*

## What a link is made of

| part | conventional shape | time | reshaped | time |
|---|---|---|---|---|
| **join**: reference -> definition | hash the name, probe a table, compare strings | 1,000-1,450 ms (260-360 ns per name) | compare 64-bit fingerprints the compiler already computed | 48-52 ms (12-13 ns) |
| | | | the same as a 4-round scatter race with SIMD gathers (Strand's own name resolution) | 93-103 ms (23-26 ns) |
| | | | the reference is `(unit, ordinal)`: nothing to join | 0 |
| **layout**: addresses | prefix sums | 3 ms | (already the right shape) | 3 ms |
| **imports**: one address per reference | random reads of a symbol table | 13-14 ms (4.5 ns) | (the same) | 13-14 ms |
| **relocation** | copy the code, then write 4 bytes per site | 78-79 ms | gather the values, then one pass that expands them into a mask of hole bytes | 61-63 ms, 37 of it my scalar clean-up of the value stream |

Fingerprinting every name once, read in order, costs 84 ms (21 ns per name, 2.7 GB/s with
an 8-bytes-at-a-time scalar hash).

## What that says

**The join is random access, and SIMD does not help it.** With strings gone, resolving a
name is one probe of a table too big for the cache: 12 ns. The scatter-race form, which is
branch-free and uses gathers for the lookups, is twice as slow as the scalar probe loop on
this CPU. That matches HARDWARE.md (a gather costs what a scalar load loop costs) and I
think it is fundamental rather than an accident of Zen 4: what bounds a join is how many
cache misses the memory system serves at once, and an out-of-order core already keeps
about as many in flight from a scalar loop as a gather does. It is data-parallel in the
sense that threads help; it is not SIMD-shaped.

**But 95% of the conventional join is not the join. It is strings.** Hashing a name is 21
ns; comparing it with the definition's copy, somewhere else in memory, is most of the other
240-340. A linker has to do this because the object file format hands it names. A compiler
has every one of those names in hand, hot, when it emits the reference.

**Relocation is already nearly a stream.** Sites are sorted by offset, so the writes march
through the output; the cost is a second pass over memory that has just been copied. Done
as one pass (code, a mask of hole bytes, a stream of values; `vpexpandb`) it is 1.25x
faster here and has no scatter in it. The values come from a per-unit table of import
addresses that fits in L1.

## The reshaping

Linking stops being a join when the inputs stop being strings and start being indices:

1. **References are `(unit, ordinal)` pairs.** The compiler resolves a name when it reads
   the other unit's interface, which it has to do anyway to check the use. The ordinal is
   the position of the definition in that interface. Nothing with a name in it reaches the
   link. (Fallback for formats that must carry names: a fixed-width fingerprint beside each,
   so the join never touches a string: 20x cheaper.)
2. **Layout is a prefix sum** over unit sizes, and over the export offsets inside each unit.
3. **Imports are one gather per reference**: `address = base[unit] + export[unit][ordinal]`.
4. **Relocation is a stream**: the compiler emits, per unit, the code, a bit mask of the
   bytes that are holes, and one import index per hole in order. The link gathers the
   values from the unit's import table and expands them into the holes.

Steps 2-4 for this job: 3 + 14 + 62 = **about 80 ms against 1.1-1.5 s**, on one thread,
and every step is a scan, a gather from a small table or a streaming blend. That is a shape
Strand can express, with one primitive it lacks (below).

**Do we need the link at all?** If code reaches other units through its import table at
run time (what position-independent code does through a GOT) step 4 disappears: the "link"
is layout plus filling the import tables, 16 ms for 250 MB of code, and the code pages are
never written, so they can be mapped straight from the compiler's output. The price moves
to run time: one indirection per cross-unit reference. And if every unit is given a fixed
region of the address space when it is created, step 2 goes as well and loading is one
mapping per unit; that trades away dense packing of code.

## What is lost

Ordinals make a unit's interface an ordered thing: reordering or removing an export
invalidates the units compiled against it, where names would have survived. Nothing here
does what link-time optimisation, section GC or identical code folding do; those are graph
problems and I have not looked for a shape for them. And the two things a real linker
spends much of its time on, reading inputs and writing the output, are untouched.

## The linker, written in Strand

The reshaped link is now a Strand program, `examples/link.strand`, 30 lines, checked byte for
byte against a plain C linker (`explore/linkjob.c ref`) on a job file both read: 2,000 units
of 64-192 KiB, 1,000,000 exports, 3,000,000 imports, 12,150,000 relocations, a 262 MB image.
The whole of it:

```
<raw                                              # the job file, as a byte vector (not copied)
...                                               # header fields and section offsets: scalar lines
~U 4 * Ousize + {raw :usz dup scan swap - >inb    # where each unit's code is in the file
usz 63 + 6 >> 6 << :al dup scan swap - >ubase     # where it goes: a prefix sum of aligned sizes
al $Tot
~U 4 * Ouexp + {raw dup scan swap - >exbase
~Ne 4 * :e4 Oexunit + {raw @ubase e4 Oexoff + {raw + >exaddr                 # every export's address
~Ni 4 * :i4 Oimu + {raw @exbase i4 Oimo + {raw + @exaddr >imaddr             # every import's: (unit, ordinal)
Tot ,img                                                                     # the image: zero bytes
~U :ui dup @inb Ocode + usz ui @ubase (raw )img end                          # one slice per unit
~Nr 4 * :r4 Orimp + {raw @imaddr r4 Orunit + {raw @ubase r4 Oroff + {raw + :pl - pl }img   # one dword per relocation
'img out
```

`examples/link2.strand` is the same linker with imports resolved by 64-bit fingerprint
instead of ordinal: exports race for slots in four tables, each import keeps the export
whose fingerprint matches. Same image.

Whole processes, the job in the page cache, output to /dev/null, median of 7, ms:

| linker | 1 CPU | 8 CPUs |
|---|---|---|
| plain C, copy and patch unit by unit | 264-268 | (one thread) |
| Strand, imports by (unit, ordinal) | 294-300 | 279-285 |
| Strand, imports by fingerprint | 397-407 | 304-315 |

What that shows, and what it does not:

- **The reshaped link is expressible, and it is short.** Every step is a line: prefix sums,
  gathers, a slice copy, a dword store. It took five new primitives (`<x`, `,x`, `(src )dst`,
  `}x`, `'x`), all of which the compiler's own back end now uses or could.
- **It does not beat the scalar C linker; it is 1.1x slower on one core.** Both spend about
  150 of their milliseconds in the kernel, faulting in the 262 MB image. Of Strand's user
  time, relocations are about 80 ms (6.5 ns each: four reads of the job, two table gathers,
  one store), the copy 40, imports 28.
- **Eight cores barely help**, because the two lines that do the work, the slice copy and
  the dword store, run on one thread: the runtime cannot know that their destinations do
  not overlap. They do not (offsets are sorted and disjoint), and that is checkable with one
  comparison against the previous element, so "an ordered sink may run in parallel when its
  destinations are increasing" is the next thing to build. An earlier version that zero-filled
  the image with a parallel line was *faster* on eight CPUs (210 ms) for one reason: the page
  faults were taken on eight threads.
- **The join costs what the prototype said**: about 110 ms more for 4,000,000 names by
  fingerprint, nothing by ordinal.
- A job this shape is bounded by memory traffic and page faults, not by arithmetic. SIMD
  buys little here; what the reshaping buys is that nothing in the link is a string or a
  hash-table probe, and that every line is one whose parallel form is mechanical.

## What this suggests for Strand

- **Slices and dword stores** turned out to matter more than the `expand` I first proposed:
  the copy is the larger part, and a dword store at a byte position is one instruction.
  Both are built (above), and the compiler's own back end uses them.
- **Ordered sinks in parallel under a monotonicity check** (above).
- **Separate compilation by slot number.** A Strand unit already refers to its vectors by
  slot; two units could share names as `(unit, slot)` and be joined by layout and expand,
  with no name table in the output. That would be a linker written in Strand whose every
  step is in the sweet spot, to measure against recompiling the whole program.
