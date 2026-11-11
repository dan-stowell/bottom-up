# Fourth audit — b591f69

## Verdict

**The implementation has made real progress, but the new facilities are not
correct for all accepted programs, and several performance explanations need
stronger controls. A fixed point and the passing suite do not establish the
advertised safety or generality.**

Reviewed `b591f699daa4bc4d7994c2b16f1891cb538ba113` against
`af3ecc935319a4698b499c66044faff95ef0663d`, November 10, 2026.
Only this report and audit evidence under `reviews/audit4*` were changed.
Experimental implementations and patched executable copies are controls, not
installed fixes.

<!-- Final findings summary and measurement tables are filled after the isolated campaigns. -->

## Findings and reproductions

### 1. Slice writes escape the alias rule and silently disagree with the interpreter

**An accepted program exits successfully with different results.** With stdin
containing 32 `A` bytes:

```text
bytes >s
32 ,d
~32 dup 1 idx (s )d 0 * 31 + [d out
```

The interpreter first copies all 32 slices, then reads byte 31 for every
element: 32 `A` bytes. Native execution copies and reads one 16-element chunk
at a time: the first chunk reads the still-zero destination at position 31.
Both executions return zero.

This is not an invalid index or a same-vector slice: both vectors are defined,
the slices are disjoint, and every read/write is in bounds. The problem is
that an in-place slice is allowed alongside reads of its destination.
`boot/spec.py` marks only the two scatter classes in `ISSCAT`; the alias checks
in all three compilers only examine a line whose **last** token is such a
scatter. A slice is a mid-line operation, so that rule does not protect it.
`E_TYPE` distinguishes element representations; it does not establish alias
safety or whole-vector evaluation semantics.

Evidence: `reviews/audit4-2026-11-10/slice-read.json`; the memory sub-audit
contains the repeatable extended checks.

### 2. Memory-boundary and lifetime checks

<!-- Awaiting completed memory sub-audit. -->

### 3. The new front end agrees extensively; its hash is not a random 64-bit identity

**No acceptance or emitted-byte divergence was found in the front-end
campaign.** This is positive evidence for the byte lexer, SWAR numbers,
generated keyword table and changes to the language definition:

* 10,141 generated cases, including all statuses 0–14, plus 891 regular-file
  comparisons.
* Another 568 boundary sources, making 2,272 comparisons across C/SIMD and
  pipe/file input. Cases include 64-byte, page, 64-KiB and 1-MiB boundaries;
  quotes, comments, NUL, EOF, long names and invalid literals.
* 9,045 valid decimals executed in 19 batches. Separately, the audit exhausts
  all 10,000 four-digit groups and all 1,110 reachable short-group reciprocal
  cases. The arithmetic agrees with ordinary decimal conversion.
* Keyword near misses, sigils and non-keyword occupants of all 256 perfect-hash
  buckets. A bucket match alone is not accepted as a keyword: the four words
  are compared.
* Constructed race/collision errors, including combinations with other errors,
  agree with the specified error priority. The witnesses were also checked
  through all three bootstrap stages.

The replacement `E_RACE` fixture is genuine. There is an even smaller witness:

```text
0 >n0001511
0 >n0004546
0 >n0011514
0 >n0014549
0 >n0021513
```

For its ten tokens the mask is 31. All five names have the four keys
`(13, 26, 16, 13)`; four rounds cannot resolve all five. Every implementation
returns 4. This is a property of the specified language, not a SIMD-only
miscompile.

**Fingerprint collisions are also easy now, without a search.**

```text
1 >bbbbdddd
2 >edddabbb
```

Both names hash to `(3044233960, 2705037177)` and all compilers correctly
return `E_COLLISION` (5). Each hash is a sum of functions of `w[j] + j`;
permuting those four adjusted words preserves both sums exactly. The audit
constructs a family of 24 ordinary ASCII names sharing one fingerprint.
This is incompatible with treating an adversarial collision as a `2^-64`
event. It does **not** demonstrate a random-input collision rate, and the
exact name comparison prevents silent name aliasing.

Thus agreement on accepted programs survived these tests, but the historical
`2^-64` rationale in REVIEWS review 2/1c no longer supports dismissing
adversarial fingerprint rejection. Keywords being outside the race is
implemented consistently. No full keyword/name hash collision was found by
the tested construction.

Reproduction and full case inventory:
`reviews/audit4-frontend/README.md`, `probe.py`, `focused.py`,
`results.json`, `focused-results.json`.

### 4. Removing the prelude broke source-location diagnostics

For:

```text
~1 @missing out
=missing
```

the runtime correctly fails because the static vector is empty. But
`python3 boot/strand.py map PROGRAM` reports its first-line tokens on **line
0**. `tools/where.py PROGRAM 4018f6` (the address in the recorded build)
identifies `@missing` on line 0 and prints a blank source line.

`analyze()` now numbers the user's first line zero (`boot/strand.py:31,36`),
while the map output and `tools/where.py:9` still rely on the removed prelude
making user lines one-based. This is a diagnostic regression, not an emitted
instruction error. Exact commands/output:
`reviews/audit4-frontend/focused-repros.json`.

### 5. Whole-compiler speed and memory

**The large improvement is real; exact single-core parity did not reproduce.**
Fifteen interleaved rounds, cached regular-file input, complete child process,
output to `/dev/null`; output bytes checked separately for every configuration:

| Source bytes | CPUs allowed | Scalar C median ms | Strand median ms | Strand/C time |
|---:|---:|---:|---:|---:|
| 128,591 | 1 | 4.875 | 5.527 | 1.13× |
| 1,033,565 | 1 | 41.148 | 44.670 | 1.09× |
| 8,292,332 | 1 | 359.983 | 394.711 | 1.10× |
| 128,591 | 8 | 8.013 | 12.995 | 1.62× |
| 1,033,565 | 8 | 56.296 | 60.456 | 1.07× |
| 8,292,332 | 8 | 368.737 | 311.509 | 0.84× |

The inputs are the current compiler source and `tools/mkbig.py`'s 8/64-copy
sources, not unrelated real-world source corpora. Current tables make these
slightly larger than the historical 964-KB/7.7-MB claims. The 1-MB “about
1.1–1.2×” description is supported. On the large single-core input Strand is
about 10% slower, not the reported 365 versus 390 ms win. It is nevertheless
much closer to scalar C than the third audit's roughly 1.6× result.

Median peak RSS on one CPU, KiB:

| Source | Scalar C | Strand |
|---|---:|---:|
| compiler itself | 6,592 | 5,224 |
| 8 copies | 42,956 | 37,852 |
| 64 copies | 333,004 | 298,316 |

Memory parity is supported, conservatively: Strand uses about 10–12% less RSS
on the two larger inputs. This is actual `wait4` peak RSS, not a prefix
difference or an allocator estimate. The large eight-CPU win is an observation
for this new revision/workload, not rehabilitation of the old revision's
withdrawn result. Small-process all-CPU runs remain particularly variable.

The source really implements the advertised byte-lane lexical pass,
per-token word gathers and slice/store code generation. There is also a
second byte automaton pass for quote counting (`src/compiler.strand:21`), so
“one automaton line” describes the main token-boundary pass, not literally all
lexing work. Slice emission really is a scalar lane loop with vector copies;
the ledger's admission is substantive. Variable output positions do not,
however, prove an inherently serial algorithm: prefix-computed nonoverlapping
ranges are parallelizable. The current implementation chooses a serial loop.

Reproduction: `python3 reviews/audit4-2026-11-10/measure_compiler.py`.
All samples, statuses, faults, RSS and source/binary/output hashes are in
`compiler.json`; `compiler.log` is the summary.

### 6. Differences of separately compiled prefixes are not per-line profiling

The method measures **different programs**, not successive execution points
of the same program. The caveat in `tools/lineprof.py:5–7` that memory reuse
differs “slightly” is insufficient.

For example, compare the prefix:

```text
input :a a + end
```

with the full program that appends `a out4`. In the prefix, `:a` and `a` are
chunk-local; there is no vector allocation. In the full program, `:a`
allocates/stores a full vector and the reread gets the persistent-tee class
that vetoes parallelism. Adding the **second** line changes the first line's
instructions, memory traffic, faults and permitted execution path. A
difference between the two cannot be attributed to the added output line.

The current compiler also changes vector lifetimes, slot ordering and
allocator reuse as prefixes grow. Differencing independently chosen fastest
samples adds timing noise and can give negative phase costs. Grouping those
differences does not remove the changed-program confound.

There is a separate harness defect: `measure()` discards `wait4`'s status.
The audit calls that exact function on `/bin/false`: five failed executions
are accepted as an ordinary measurement. Therefore even “the last prefix's
totals are real” needs a successful-execution check. The times are child
user+system CPU time, including the forked Python child's setup, not elapsed
wall time or a direct measurement of instructions in the named line.

For a quantitative control, the audit compiles the full compiler once and
patches **copies** to exit just after selected line boundaries. Earlier code,
allocations, local/persistent decisions and releases remain those of the full
program. These are compared with the separately compiled prefixes, with
checked exits and randomized order.

At 1,033,565 input bytes, nine interleaved samples per control:

| Stop after compiler line | Prefix median ms | Stopped-full median ms | Prefix / full minor faults |
|---:|---:|---:|---:|
| 19, main byte lexer | 2.168 | 2.600 | 679 / 936 |
| 37, token spans | 7.718 | 13.078 | 2,861 / 5,751 |
| 42, name words | 9.655 | 16.365 | 3,502 / 7,035 |
| 59, keyword recognition | 16.295 | 21.573 | 5,431 / 8,001 |
| 100, name resolution/checks | 24.903 | 28.393 | 7,420 / 9,025 |
| 202, completed code buffer | 42.190 | 42.475 | 9,132 / 9,133 |

At the name-word boundary the prefix has **half the faults** of the actual
full-program execution to that point. Later totals converge; that does not
repair the attribution of the earlier work. Running the stock profiler also
produced a negative “slots by one scan” section, −0.26 ms, and −1.37 ms for
line 23. These are not negative execution costs.

The helper is useful for generating hypotheses about expensive regions. It
does not justify precise per-line attribution or causal claims about page
faults. Use full-binary instrumentation or sampled profiling, retain all
samples, check statuses, and corroborate any proposed optimization by a
whole-program measurement.

Reproduction: `check_method_and_backlog.py` and `measure_compiler.py` under
`reviews/audit4-2026-11-10/`; the latter changes only generated audit copies.

### 7. Link-operation prototype: distinguish reshaping the job from accelerating it

**The generated full job is byte-correct. The broad causal claims are not
established by the original experiment.** The document does explicitly call
this a synthetic core-operations prototype and list major omitted linker
features; those disclosures should not be erased when criticizing it.

The original exact-name path hashes strings and then follows a separated hash
array, ID array, offset array and definition bytes. The fingerprint path
removes hashing, offset dependencies and exact-byte checks. Their subtraction
does not show that **95% is string handling**, nor isolate `memcmp` from random
dependent accesses and cache/page state. The later sequential 84-ms hash scan
is not the cost of hashing inside the earlier interleaved join. The reported
“12 ns” divides insertions plus lookups by four million; it is not one measured
probe latency.

There is also a concrete conventional-baseline defect: default GCC
fortification makes the original four-byte relocation `memcpy` a
`__memcpy_chk` call per site. The variable-length 1–7-byte tail of `hash_str`
also calls it. Controls retain the same flags and hash results while spelling
the stores as four byte assignments and tail loads as fixed 4/2/1-byte copies.
Other controls retain exact comparisons but change prehashing and table layout.
These are not handwritten SIMD linkers.

<!-- Insert isolated prototype timing table. -->

#### The “scatter race versus scalar probe” is not an isolated SIMD experiment

`explore/link.c:66–87` compares different algorithms and layouts. The scalar
linear table holds hashes with IDs; the four-table race stores IDs and needs
a second dependent fingerprint gather. It initializes 32 MiB of slots rather
than 8 MiB of IDs, and construction actually uses **scalar conditional stores**,
not SIMD scatter. Construction, initialization and lookup share one clock.
The scalar table's hash+ID footprint is 24 MiB, not demonstrably beyond the
reported 32-MiB L3; actual effective cache occupancy requires measurement.

The audit therefore compares scalar and gather lookups over **identical**
four-table data and reports build/lookup separately. Whether this particular
gather loop wins or loses cannot establish the universal claim that SIMD
cannot help joins, or that an OoO scalar loop already saturates available
memory misses. No miss/concurrency counters were supplied.

#### Relocation can be a stream; the proposed special format is not required for locality

The original conventional path copies the entire 250-MiB image, then patches
it. Copying and patching one unit or cache-sized block at a time is a natural
scalar alternative, already used by the later `linkjob.c`. The expansion path
also needs a 48-MB value array and 32.8-MB mask; mask production is outside the
clock. That exclusion is disclosed but still matters to an end-to-end claim.
The “37 ms scalar clean-up” label is wrong: its timer includes **all** value
gathers and arithmetic as well as cleanup.

The 80-ms number sums kernels with precomputed identities/masks, prefaulted
output and different warm states. It is not process wall time and excludes
producer work and I/O. Resolving `(unit, ordinal)` at the producer genuinely
removes the linker's name join, but changes the interface contract and moves
work. It does not preserve all the omitted production-linker semantics.

#### Validation and domain limits

* A 579-reference control exposes three ignored tail references: both the
  original SIMD loop **and its comparison** round down to eight. The original
  still prints zero unresolved references. The actual three-million-reference
  fixture is divisible by eight and passes independent checks.
* The original exits zero even after printing `MISMATCH`; the audit driver
  checks both status and validation text, and adds independent reference-ID
  and byte-wise relocation oracles.
* A fingerprint-only path does not implement exact names. Forced 8-bit
  fingerprints demonstrate silent wrong answers, whereas exact paths remain
  correct. This tests collision policy, not natural 64-bit collision frequency.
  Neither missing-name termination nor race fallback is a general facility
  in the original prototype.
* The seeded full fixture has **zero duplicate relocation sites**, despite the
  cleanup discussion. Smaller controls deliberately exercise duplicates.
  Inspection confirms the original output-prefaulting stores and comparison
  work survive optimization; dead-code elimination was not the explanation.

Detailed source references, disassembly, control matrix, sanitized small
checks, full-image checks and reproduction:
`reviews/audit4-link-prototype/README.md`. It separates resident, evicted and
fresh-output-page regimes and keeps raw randomized trials.

### 8. The Strand linkers and the scalar reference

**Byte-identical images reproduce on the generated full job, including against
an independent Python oracle—not merely the C reference.** The ordinal version
also passes all 92 small cases. Both linkers were exercised on one/eight CPUs,
including eager parallel test mode, with boundary sizes, negative relative
displacements, alignment padding and variable export counts. There are 1,840
small control runs plus the full-image comparisons.

The C reference has a reasonable algorithmic structure for this **ordinal
job**: mapped input, an import-address table, ordinary unit copy followed by
unit-local patches, and one materialized output image. It is not a
string-resolving production linker. But its compiled inner loop is a weak
baseline on this VM: `gcc -O2` injects `_FORTIFY_SOURCE=3`, producing a
`__memcpy_chk` call and dynamic bounds calculation for **each relocation**.
`-O3 -march=native` alone does not cure that.

The strongest minimal control changes only that four-byte copy to four
explicit little-endian assignments, with the **same original flags**. GCC
emits one dword store; unit copying stays fortified. Other controls disable
fortification, add an explicit per-field in-unit bound, resolve fingerprints
by scalar probing, parallelize independent units, or stream unit output.

<!-- Insert isolated whole-linker measurement table. -->

The byte-store/no-fortify controls change checking policy; that must be
disclosed. Neither original C nor these controls validates the entire job as
untrusted input. The explicitly checked control retains an inline in-unit
bound stronger than merely checking the complete output allocation.

A unit-streaming C alternative writes identical stdout without retaining a
complete image. It is relevant to a streaming-output API, **not** a like-for-like
full-memory-image baseline. `/dev/null` avoids a real consumer copying/storing
output. Strand also copies through its 256-KiB output buffer, whereas original
C writes the materialized image directly. Those differences preclude assigning
the whole measured gap to vector arithmetic.

#### Fingerprint generality fails even without duplicate fingerprints

One unit, five exports/imports, and these distinct fingerprint halves suffice:

```text
low:    0  4096  8192  12288  16384
high:   0     0     0      0      0
```

All four placement keys collide for every export. `link2.strand` correctly
detects the unplaced export and exits **9**, while the ordinal and scalar
probing implementations produce the expected image. Removing all imports and
relocations still causes failure. The format needs a placement-success
precondition or a fallback; “unique fingerprints” is insufficient.

Two exports with **equal** fingerprints instead produce a wrong image with
exit zero: the first matching fingerprint wins, without checking identity.
This is outside a stipulated unique-fingerprint domain, but the unqualified
“same image” claim needs that restriction. The production generator exercises
only 62 fingerprint bits (31 per half); the audit's additional full-width
cases passed, so this is a coverage limitation, not a high-bit miscompile.

#### The documented full-job counts do not match the current generator

`linkjob gen 2000` produces:

| Quantity | Current exact value |
|---|---:|
| Relocations | **12,644,906**, not 12,150,000 |
| Raw code bytes | 265,608,716 |
| Aligned image bytes | **265,668,544** |
| Job bytes | 481,363,608 |

The regenerated input matches the preexisting `build/job2000`. Its SHA-256 is
`32571e91737f9ce7cb4ee43077087b46bd09f183833cdacb50bcb48e240b8588`;
the independently expected image is
`69e3ce48007c19a0aee9a78c9aeee96ce76743e2b09344228fa4f02fe6181ba6`.
The document has no historical input hash/raw samples tying its counts to its
timings. This is a provenance discrepancy, not evidence of fabricated results.

The original C assumes relocation records are grouped by unit; a supplemental
ungrouped job breaks it but not Strand. This lies outside the document's
sorted benchmark, though the file-format comment does not state the
restriction. Sorted overlapping writes also pass the audit's input-order
oracle, but are outside the disjoint performance workload.

Finally, LINKING's proposed test of **increasing destinations** is not enough
to authorize parallel writes: four-byte fields starting at 0 and 1 overlap.
It needs increasing **nonoverlapping intervals** (`next >= previous + 4`, or
the previous slice end). Current copy/store lines veto the pool, so this is a
defect in the proposed condition, not an observed current parallel race.

Commands, exact source-only replacement, disassembly, input/oracle generators,
all correctness results, source/binary hashes and raw timing samples:
`reviews/audit4-linkjob/README.md`.

### 9. Review 6 is not a complete accounting of the third audit's backlog

The four explicitly deferred items remain deferred:

* Packed-transition ordinary C was not added to the benchmark candidates.
  `bench/base.c`, `base_omp.c` and `hand.c` are unchanged since `af3ecc9`.
  SWEETSPOT's multicore automaton ratios still use the known weaker ordinary-C
  formulation. The third audit's approximately 1.8–2.8× result against the
  stronger baseline remains the relevant qualification, not a newly measured
  ratio in this audit.
* `tests/fsm_all.py` still overstates composition/position coverage. Recounting
  covers 4,096 of 65,536 ordered function pairs, and 16,350 of 16,384
  class-pair/offset combinations. Its normal 103,022-byte run is still below
  the pool threshold.
* The `avxvnni` checker still uses generic `vpdp`, which does not distinguish
  the intended VEX probe from the different EVEX form.
* There is still no dependent-carry retry performance campaign. Path assertions
  are correctness/path evidence, not a measurement of the retry cutoff's cost.

The phase table now correctly calls JSON structural extraction, acknowledges
token positions and number parsing, and describes the compiler's liveness as
last occurrence rather than a gen/kill scan. The new byte bridge is real.
But Review 6 omits other unresolved findings/qualifications:

* The duplicate-member JSON oracle defect remains:
  `jsongen.skeleton(b'{"x":[],"x":{}}')` returns `b'{:{}}'`, losing the first
  member. The unique-key timed generator avoids it; arbitrary valid JSON does
  not.
* `bench/oneoff.py` still selects `glob(...)[0]`, reuses unhashed prefix files,
  runs grouped trials and retains no individual timing samples or output
  comparison. Those were not fixed.
* Historical blog 04 still says no byte-line positions and “everything after
  the lexer” is outside the model. The repaired phase table does not update
  that text. SWEETSPOT's whole-compiler phase row also retains the old
  1.3–1.5× slowdown, inconsistent with the new performance account.
* Still-relevant scope qualifications (not necessarily unfixed defects): the
  distinction between these hand-C implementations and an expert ceiling,
  the stronger JSON hand-C control, single-VM/distribution/output limitations,
  the concatenated-document fixture, and the hardware gather working-set
  qualification still matter.
* Independent acceptance semantics, resource-limit coverage, broader fixture
  distributions/CPU counts/output consumers, uncertainty estimates and better
  timing provenance remain work, not completed responses. This audit adds a
  collision reproduction; the production suite still has no such fixture.
  The third audit's proposed all-starting-state/position/production-boundary
  automaton checks and independent pool-veto validation also remain open.
  So does a generally useful lexer/token-stream benchmark checked independently,
  including invalid input; the new compiler's real lexer is not that benchmark.

Consequently Review 6 is not a complete standalone closure matrix for
`AUDIT_3.md`. Its four-item list is accurate but incomplete; some other caveats
remain documented in earlier reviews rather than literally being deleted.
The older audit remains necessary context. The default pool/path suite still
passes, and this diff did not replace the carry-verification algorithm.

Evidence: `method-and-backlog.json`, `fsm-coverage.json` and the unchanged
source hashes under `reviews/audit4-2026-11-10/`.

## Reproduction record and limits

The default `./test.sh` passes: 138 hand cases per bootstrap stage, 400
compile-fuzz cases per stage, semantic fuzzing, 4,836 pool executions, 80 path
checks, 48 all-function executions, both small linker comparisons and the
large compiler comparisons. Stage 1 = stage 2 = stage 3:

```text
818d4b7202579f00f1b154741709d1fddd3c169d6f2078579e66eb63ebc0c730
```

This is the same eight-vCPU EPYC 9554P/KVM environment, GCC 13.3.0.
Correctness campaigns and performance campaigns were separated; timing
campaigns ran sequentially with the other audit workers paused. Guest
quiescence does not remove host/VM variation. Output equality was checked
outside timed `/dev/null` samples; failed timed children were rejected.
The stock prefix profiler's unchecked status is the exception being audited,
not the method used for the controlled timing tables.

No exhaustive correctness proof, exploitability claim or production-linker
benchmark is implied. The interpreter shares Python front-end analysis;
native compilers share stencils. Independent decimal and linker oracles
improve coverage without removing those dependencies.

<!-- Final command index and prioritized follow-ups pending. -->

## What I would investigate next

1. Fix and specify slice/read aliasing before extending parallel sinks. Add
   successful-execution differential cases across 16/64-byte chunks and worker
   boundaries, rather than relying on bounds-failure fuzzing.
2. Establish a real allocation/ownership invariant for every byte-vector
   origin, including mapped input and zero-length/uninitialized vectors.
   Test physical access widths with guards, not just logical output masking.
3. Retain exact-name collision checks and explicit failure semantics. Test
   adversarial fingerprints/race exhaustion as ordinary cases; do not rely on
   a random-hash argument for a structured sum.
4. Profile the full binary, then remeasure whole-program changes. A difference
   of prefix compilations is not a substitute for preserving code/lifetimes.
5. Strengthen and document the scalar linker baselines, include the cost moved
   to producers, and distinguish materialized-image linking from streaming
   output. Compare the same join algorithm before attributing a difference to
   SIMD.
6. Carry forward the complete third-audit backlog and rerun the published
   automaton table with the packed ordinary-C alternative.
