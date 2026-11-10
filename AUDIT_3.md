# Third audit — af3ecc9

## Verdict

**The compiler remains a valid working implementation, the carry-verification
change has a sound correctness argument, and the main performance result
reproduces. This is not blanket validation of the claims.**

Reviewed `af3ecc935319a4698b499c66044faff95ef0663d`, November 10, 2026,
against `17e317d`. In particular:

* **No successful-execution compiler miscompile was found.** The complete test
  suite passes. Additional adversarial carry tests pass, including permutations,
  non-injective automata, maxima, overflow, deep compositions and path detection.
  The earlier chained-`prev` counterexample now really stays parallel.
* **The new hand-C column is substantive, not a scalar strawman.** It implements
  the advertised vector algorithms, and the one-CPU “75–95% of hand C with the
  same technique” description is supported by both retained and fresh samples.
  It is nevertheless an implementation comparison, not an expert-performance
  ceiling.
* **There is another ordinary-C source-form improvement.** Packing the four
  transitions into one byte substantially improves the multicore
  `strip`/`idents`/`strings` baselines, without intrinsics or changing I/O,
  partitioning or the two-pass algorithm. On eight CPUs, Strand's advantage over
  the stronger ordinary C is about **1.8–2.8×** in two batches, rather than a
  general 3–5.6× for these tasks. The advantage survives; its size is not
  intrinsic to “ordinary C.”
* **Some evidence is described too broadly.** The all-256 test does not cover
  all function pairs or every claimed pair/position. JSON structural extraction
  is not a complete scanner or parser. Several entries in the compiler-phase
  map misdescribe functionality the self-hosted compiler already implements.
* **The named I/O and hardware fixes are real.** The I/O loads are observable
  again; missing probes are rejected; the earlier contradictory hardware
  conclusions are substantially corrected. One narrower instruction-encoding
  regression-check gap remains.

No compiler, runtime or benchmark implementation changes are included in this
audit. Experimental C alternatives are copies under `reviews/`, not installed
fixes. All timing campaigns ran sequentially, with other audit work stopped.

## Findings and reproductions

### 1. Carry verification is a correctness certificate, not a convergence promise

**Assessment: the relaxation is justified; no false acceptance found.**
The important distinction is between deriving a good guess and checking that
the resulting execution is correct.

`stencils/stub.S:539–552` saves the true incoming carries for the first range.
After the real pass, `:585–638` checks every used carry at every boundary.
The state that can affect subsequent execution is:

| Carry | State actually needed | New comparison |
|---|---|---|
| Dword `prev` | Last dword of the previous chunk | Exactly that dword |
| Byte `prev` | Last byte of the previous chunk | Last **dword**, conservatively checking three extra bytes |
| Automaton | The accumulated function's value at state 0 | That value |
| Sum / maximum | Running value | All carry dwords, as before |

See the consumers in `tools/mkstencils.py:140–147,328–332,406–431`.
For a verified execution, the first range starts correctly, its effective
outgoing state equals the next range's effective incoming state, and so on.
Induction over ranges therefore gives the serial result for the whole composed
line. An upstream stream that was wrong inside a range cannot evade this
argument merely because a downstream sum happens to cancel: the upstream
carry's incoming state is also checked.

This argument applies to arbitrary compositions **within the existing
pool-safety rules**. It relies on deterministic stencil semantics and the
vetoes on scatter, compress-to-memory and cross-range persistent-tee rereads.
It is not a formal verification of the entire compiler or all its safety rules.
Failed real-pass checks trigger fallback; output is committed after verification
(`stub.S:583–584,707–739`). Verification is exact state comparison, not a
probabilistic hash.

The retry algebra also checks out:

* **Sum:** `IN_new + OUT_old - IN_old` recovers the old pass's contribution
  modulo 2^32. It becomes the correct contribution once its upstream inputs are
  correct.
* **Automaton:** let `P_e(s) = s xor e`. The real pass starts from this
  permutation and reports `M o P_e`. The retry computes
  `(M o P_e) o (P_e o IN_new) = M o IN_new`.
  **M need not be invertible.** Constant and non-injective transitions are not
  counterexamples; the invertible object being removed is `P_e`, not M.
* **Maximum:** an over-high seed can hide the range's maximum. A low-seeded
  pass exposes it, and the following derivation propagates it. That exploratory
  pass may be wrong; acceptance still requires boundary verification.

The retry guard is `old_retry + 4 <= nthreads` (`:641–646`): zero retries on
2/3 CPUs, one on 4, five on 8. That supports the stated two/six-level budget for
the discussed sum/automaton chains, with an extra pass for a missed maximum.
It does **not** promise parallel execution for every program, input or source
depth. Pool vetoes still apply; lucky guesses/cancellations can change the path.
“One thread is as fast” after this budget is a heuristic, **not** something
established by counting passes. The document correctly admits that dependent
carry speedups have not been benchmarked.

The one-chunk `prev` warm-up has a real bound: accepted lines have at most eight
`prev` operators because repeat use of a carry group at one stack depth is
rejected. Eight one-element delays fit in a 16-dword or 64-byte chunk.
For nonlocal carries the warm-up's incoming state is also derived and its
effective result checked.

**Reproduction:** `python3 reviews/audit3-carries/probe.py`.
There are 104 deterministic programs and 540 correctness comparisons:
small inputs against the scalar interpreter, production controls against
single-CPU native execution. Of 436 multicore configurations, the patched
`fsbad -> exit_group(77)` detector observed 190 safe fallbacks and 246 executions
without fallback. Every unmodified result was correct.

In particular:

```text
input 1 & scan dup prev dup prev + + out4
```

on 524,289 bytes of `0x01` now succeeds **without fallback on 2, 3, 4 and 8
CPUs**. Conversely, the retained `max-high-seed` case still falls back on
2/3/4 and succeeds in parallel on 8, correctly in all cases.
`reviews/audit3-carries/README.md` gives the full invariant, sources and hashes.

### 2. “All 256 functions” is true; the claimed composition coverage is not

`tests/fsm_all.py` is useful additional testing, but its docstring and Review 5's
“functions and their compositions: done” need qualification:

* The 256 functions are divided into 16 disjoint groups of 16. Only
  **4,096 of 65,536 ordered function pairs** can occur in those tables.
  Cross-group pairs never occur. Within each group the state-2 and state-3
  mappings are fixed.
* The fixture contains **16,350 of 16,384** possible
  `(class A, class B, offset modulo 64)` combinations, not every combination.
  For example `(0,10,33)` is absent. The shifting/padding construction is not
  exhaustive; 34 combinations remain absent even after the random suffix.
* Its input is **103,022 bytes**, below the normal 524,288-element pool
  threshold. The ordinary “all CPUs” execution is serial. The eager execution
  exercises the pool, but this test does not assert absence of fallback.
* One automaton followed by `prev` is not a test of arbitrary cascades.
  The separate pool tests and audit probes provide additional coverage.

These are **coverage defects, not observed wrong outputs**. The default suite
passes 48 all-function executions, 4,836 pool executions and 80 path checks.

**Reproduction:** `python3 reviews/audit3_coverage.py`.
The exact missing combinations/counts are retained in
`reviews/audit3-2026-11-10/fsm-coverage.json`.

### 3. A credible hand-C baseline, but still room to strengthen the comparison

#### What is fair

`bench/hand.c` really uses byte permutes for xlat, compress for filters,
transition-function scans for the toy automata, and carry-less multiplication
for quote parity in CSV/JSON. The comparison now uses matched cached-file,
whole-process boundaries, output conventions and CPU budgets. These are
meaningful implementations of the tasks, not instructions inserted merely to
make a “hand SIMD” label.

The new xlat table agrees exhaustively with Python `cp037` decoding followed by
Latin-1 encoding for all 256 byte values. It is no longer arithmetic ASCII
swapcase disguised as a general table task. The input distribution remains
mostly ASCII rather than realistic encoded EBCDIC text; that limits workload
generality, not table correctness.

There are **eight**, not the seven stated in Review 5/Addendum 4, hand kernels.
Also, “every program where ordinary C loses” has the explicitly reported
single-core `maxline` exception: there is no hand `maxline`.

#### A reproducibly stronger ordinary-C formulation

The supplied toy-DFA baselines use switches, including four independent state
evaluations per byte in the parallel summary. An audit-only replacement encodes
all four transitions in one byte:

```c
/* PACKED[c] = f(0) | f(1)<<2 | f(2)<<4 | f(3)<<6 */
s = (PACKED[c] >> (2 * s)) & 3;
```

This changes `K_strip_step` and adds its constant table; it keeps the same
semantics, two passes, ranges, buffers, output and compiler flags.
There are no intrinsics or special SIMD pragmas.

Two independently executed interleaved batches used nine and fifteen rounds,
checked every timed configuration's output, and retained every sample.
The **second batch**, median input GB/s on an eight-CPU budget:

| Task | Best supplied ordinary C, ≤8 CPUs | Packed ordinary C, 8 | Strand, 8 | Strand / supplied → stronger C |
|---|---:|---:|---:|---:|
| strip | 1.02 | 2.09 | 5.61 | 5.49× → **2.68×** |
| idents | 1.28 | 2.23 | 4.42 | 3.46× → **1.98×** |
| strings | 1.04 | 2.20 | 4.87 | 4.70× → **2.22×** |

The first batch's corresponding ratios against stronger C were
**2.52× / 1.83× / 2.78×**. This is not an isolated favorable sample.
On one CPU the packed form helps `idents` modestly and hurts `strip`/`strings`;
the right comparator keeps both formulations, rather than replacing every
switch unconditionally.

Thus the printed ratios are valid for the supplied baselines, but not a
task-level “ordinary C cannot do better” boundary. Unlike the first word-count
finding, this does **not** erase the remaining Strand advantage.

#### The hand C is not fully optimized either

The output kernels repeatedly enter OpenMP regions around 4 MiB blocks.
That means repeated region/barrier overhead, **not necessarily new OS threads**.
State summaries reread the input before the output pass. Per-thread output
buffers reserve full-block capacity rather than their range size.
These are plausible optimization opportunities, not proof of their timing cost.

I tested two such opportunities rather than assuming:

* Keeping the team outside the block loop gave **mixed, noisy results**.
  It does not establish that repeated regions explain the Strand/hand gap.
* A JSON control computes both candidate punctuation outputs during one
  quote-parity scan, then selects the correct one from range-entry parity.
  It avoids the full summary reread at the cost of extra compression, stores and
  buffer capacity. On eight CPUs its median was **9.49 versus 7.42 GB/s**, then
  **8.14 versus 7.35 GB/s** in the repeat. It was slower on one CPU.
  The improvement is useful evidence of headroom, not a universal best algorithm
  or a claim of statistical significance for every small multicore difference.

**Reproduction:** sources and correctness controls are under
`reviews/audit3-hand/`; timed by `reviews/audit3_controls.py`.
The production files were not edited. Full samples are
`controls.json` and `controls-repeat.json`.

**Interpretation:** keep “these hand-written implementations,” not “the expert
limit.” The new bracket is substantially more honest than the old strongest-C
claim. Statements such as “no compiler turns a state machine into a scan” or
“quote parity beats any automaton” still exceed what this one GCC/VM comparison
can establish.

### 4. The headline ranges match the retained data; a fresh run supports the trend

Before running anything I preserved `build/suite/results.json` as
`reviews/audit3-2026-11-10/claimed-results.json`. Recomputing ratios directly from
its samples, including the “best among sampled configurations using at most
this budget” rule, supports the rounded ranges in SWEETSPOT.

A fresh directory regenerated all five inputs and rebuilt all implementations.
**All five input hashes match the retained campaign**, including the changed
integer distribution and new JSON fixture. All 23 programs passed every
configuration's output comparison. The compiler hash also matches.

| Class | Retained Strand / ordinary C range | Fresh range |
|---|---:|---:|
| Maps, reductions, neighbours, prefix (excluding maxline) | 0.72–1.32× | 0.73–1.27× |
| xlat | 1.64–2.33× | 1.71–2.28× |
| Filters | 1.40–3.78× | 1.37–3.64× |
| strip, idents, strings | 3.04–5.56× | 2.76–6.07× |
| CSV/JSON | 2.22–7.29× | 2.10–5.52× |

These are **supplied-baseline** ranges, before adding the stronger packed C.
The one-CPU same-technique comparisons against hand C are **0.764–0.931×**
retained and **0.768–0.912×** fresh. Thus “75–95%” is a reasonable description
of those cells, not an invented range.

Selected fresh ratios, in order **1 / 4 / 8 CPUs**:

| Task | Strand / ordinary C | Strand / hand C |
|---|---|---|
| swapcase | 1.07 / 1.13 / 1.06 | — |
| xlat | 2.28 / 1.71 / 1.77 | 0.88 / 0.72 / 0.79 |
| letters | 3.64 / 2.58 / 2.48 | 0.89 / 0.79 / 0.81 |
| filter4 | 1.71 / 1.50 / 1.37 | 0.91 / 0.90 / 0.91 |
| strip | 2.89 / 6.07 / 5.04 | 0.80 / 0.97 / 0.81 |
| csv | 4.04 / 2.35 / 2.10 | 0.45 / 0.51 / 0.54 |
| idents | 3.86 / 5.41 / 2.76 | 0.83 / 0.76 / 0.61 |
| strings | 3.26 / 5.52 / 4.03 | 0.77 / 0.92 / 0.73 |
| json | 5.52 / 5.52 / 4.41 | 0.65 / 0.58 / 0.59 |

The isolated JSON 1.19× win over hand C did not reproduce. Nor did every
multicore endpoint; the repeated control batches also show large variation.
The robust conclusion is faster than these ordinary-C loops, generally behind
these hand implementations—not fixed 1.4–7× or 0.4–1.2× constants.

The main timing harness now measures what its narrow description says:
fork, child setup/exec, mapping or reading, faults, computation, writes to
`/dev/null`, exit. It validates each configuration separately and rejects every
failed timed child. It does **not** hash every timed execution, measure storage
throughput, deliver output to a real consumer, or isolate SIMD arithmetic.
“Best at budget n” considers sampled **1/4/8** configurations, not all possible
thread counts up to n.

Retained metadata describes a dirty `e5c673c` working tree and explicitly admits
that JSON was merged from a second invocation. That is weaker provenance than
immutable per-row sources/binaries, but not evidence of a bad number. The fresh
campaign records `af3ecc9`, clean tracked implementation files, input/source
hashes, GCC 13.3.0 and actual masks `[3]`, `[0–3]`, `[0–7]`.

**Reproduction:** `python3 reviews/audit3_measure.py`, then
`python3 reviews/audit3_ratios.py`. The first script only redirects benchmark
output/fixture paths; it does not change kernels or timing methodology.
See `suite.log`, `suite-results.json`, `environment.json`, and `ratios.json`.

### 5. JSON is a useful independently checked task, not a complete scanner

The generated unique-key JSON documents have a genuinely independent
structural-output oracle: parse with Python, then walk the resulting tree.
Additional tests made **1,017** parsed-tree comparisons of valid JSON, including
20 seeds, escape runs, SIMD seams, worker seams and 4 MiB seams. All passed.
Another 1,746 comparisons cover the other hand kernels. The xlat exhaustive
check and 723 checks of experimental controls also pass.

What the JSON programs emit is only `{ } [ ] : ,` outside strings. For example:

| Input | Strand output | Status |
|---|---|---:|
| `{"x":1}` | `{:}` | 0 |
| `{"different":false}` | `{:}` | 0 |
| `{"x":}` | `{:}` | 0 |

The last input is rejected by Python's parser. This is **expected** for
structural extraction, not a compiler bug, but contradicts describing the task
as a “complete scanner for a real grammar” (`SWEETSPOT.md:244`).
No token values, token positions, number/literal validation, nesting validation
or complete lexical stream is produced. CSV counts quote-external separators;
it is not a complete CSV parser either. Addendum 4's narrower “extracts the
structural characters” wording is appropriate.

Two further scope limits matter:

1. At 256 MiB the fixture concatenates **16 independently valid JSON
   documents**, not one parser-valid document. The actual file has 268,435,936
   bytes; throughput correctly uses its real length. Concatenated skeletons are
   the right oracle for this extraction stream, not evidence that the whole
   file is accepted by a JSON parser.
2. `jsongen.skeleton()` loses duplicate object members through `json.loads`'
   default dictionary construction. On `{"x":[],"x":{}}`, which Python accepts,
   it returns `{:{}}`, while all the scanners emit `{:[],:{}}`.
   The timed generator uses unique names, so this does **not** invalidate the
   published output checks. It limits reuse of that oracle on arbitrary JSON.

The hand bitmask algorithm additionally assumes valid JSON: it suppresses
backslashed quotes even outside strings. On the malformed 64-byte input
`b'\\"{' + b' '*61`, hand C emits `{`, while the three DFA versions emit
nothing. The audit found 18 such differences among 234 malformed-input checks,
and **no valid-JSON difference**. Do not report these as valid-input miscompiles;
do retain the precondition when comparing the implementations.

**Reproduction:** `python3 reviews/audit3-hand/checks.py` and
`python3 reviews/audit3-claims/check_claims.py`.
The earlier toy-grammar objection is now properly acknowledged in the main
documentation: Python source is a byte distribution, not the recognized language.

### 6. Compile-plus-run latency reproduces, with the disclosed harness boundary

Fresh `bench/oneoff.py` measurements, one allowed CPU, median of seven, ms:

| Program | Input | Strand | GCC base.c | GCC OpenMP |
|---|---:|---:|---:|---:|
| words | 1 MiB | 5.2 | 56.0 | 69.3 |
| words | 256 MiB | 35.7 | 92.2 | 105.9 |
| strip | 1 MiB | 4.7 | 49.1 | 93.3 |
| strip | 256 MiB | 85.8 | 335.1 | 351.7 |
| json | 1 MiB | 3.8 | 52.5 | 89.4 |
| json | 256 MiB | 92.0 | 527.4 | 566.7 |

These are close to the claimed table. Both compilation and execution really
happen inside the measured interval; subprocess failures are checked. Both
emit executable files before running them. This is a meaningful advantage for
these one-off commands, assuming installed/prebuilt compilers and a warm input.

It is **not** a 4–5 ms compiler-internal measurement: Python orchestration,
file opens and two `taskset`/subprocess launches are included. The text discloses
this. The small-input result is not evidence that execution alone is ten times
faster, nor a comparison with interpreting the query or using an existing
precompiled tool. Hand-C compile-plus-run is not included.

This helper is less rigorous than `run.py`: grouped rather than interleaved
trials, no retained individual samples or output comparison, and stale-fixture
risks. Specifically, `oneoff.py:24` picks `glob(...)[0]` and `:28–29` reuses
unhashed prefix filenames. Changing fixture generators does not necessarily
refresh these inputs. The audit used a fresh directory, so that defect did not
affect this reproduction. Main-suite code fixtures also depend on installed
stdlib contents, which are not included in the generator filename hash;
recording actual input digests is therefore still important.

**Reproduction:** the oneoff portion of `reviews/audit3_measure.py`;
all nine rows, including 16 MiB, are in `oneoff.log`.

I also repeated the **whole-compiler** comparison in a separate isolated,
15-round randomized batch, checking output equality under each CPU mask:

| Source | Allowed CPUs | Scalar C median ms | Strand median ms |
|---|---:|---:|---:|
| Current compiler, 100,837 bytes | 1 | 3.744 | 6.013 |
| Same | 8 | 9.044 | 14.257 |
| 64-copy source, 6,513,542 bytes | 1 | 276.705 | 426.231 |
| Same | 8 | 284.877 | 398.441 |

The slowdown conclusion stands. The one-CPU medians are **1.61× and 1.54×**
slower, slightly beyond the prose's 1.3–1.5× range. There is no resurrected
multicore win over scalar C. Small-process all-CPU timings were especially
variable (C/Strand bests 3.997/6.133 ms); neither the median nor best should be
represented as pure compiler computation. Reproduce with
`python3 reviews/audit_compile.py --reps 15 --output reviews/audit3-2026-11-10/compiler.json`.

### 7. The compiler-phase map needs implementation-specific corrections

I checked every row, not just the positive headline. Detailed code references
and small compilable controls are in `reviews/audit3-claims/phase-map.md`.

| Phase | Audit assessment |
|---|---|
| Comments and strings | Yes for the stated toy four-state grammar. No complete real-language lexer demonstrated. The exact “C needs 9 states” lacks a recognizer/dialect/translation-phase specification here. |
| Token boundaries and kinds | Expressible for restricted grammars; the compiler itself does it. `idents` measures a count of word starts, not a complete token/kind stream. |
| Complete JSON/CSV scanner | **No**, as that phrase is normally read; structural extraction/separator counting only. |
| Token positions | Missing **byte-automaton-to-dword bridge is real**. Positions in general are not absent: `input 32 > dup prev 1 ^ & idx swap outif4` emits 0 and 4 for `one two\n`; the compiler already compresses token positions. |
| Identifier/keyword interning | Implemented with bounded hashing, scatter races and exact comparisons. Not a general name-resolution or join engine. |
| Number literals | A general segmented affine-scan primitive is absent, but **decimal literal parsing is already built**, using positional powers of ten and prefix differences (`src/compiler.strand:44–50`). “Not built” is misleading. |
| Bracket depth | Dword prefix sum exists; no direct fused feed from the byte DFA. No string-aware bracket-depth application demonstrated. |
| Matching brackets/tree construction | No general implementation or stack/tree primitive shown. “Not implemented” is supported; impossibility for all bounded/unrolled array formulations is not proved. |
| Name resolution/type checking | Flat symbols and Strand's simple checks exist; general scope/type/graph algorithms are not supplied. |
| Dataflow optimisation | No general graph fixpoint engine. **The claim that this compiler performs backward gen/kill-scan liveness is false**: it recognizes the scatter-selected last occurrence (`src/compiler.strand:162–164`). |
| Instruction selection/encoding | Yes in the restricted preassembled copy-and-patch sense; not general DAG/tree instruction selection. |
| Layout/relocation | Yes for its ELF/stencil layout. The whole-compiler timing is not a phase-specific performance measurement. |

Thus blog/04's “everything after the lexer ... outside the model” is too broad:
the compiler already implements symbol handling, validation, selection and
layout. Likewise “joins outside the model entirely” conflicts with the stated
keyword self-join. Distinguish missing primitives, missing applications, missing
performance evidence and true expressibility limits.

Two four-state automata in cascade can indeed have up to sixteen joint states.
That does not demonstrate an arbitrary sixteen-state automaton or a real-language
lexer. The documentation appropriately calls the real-grammar cascade untested.

### 8. Supporting probes: prior defects fixed, one smaller checker gap remains

**I/O fix confirmed.** The documented `gcc -O2 -pthread` build's `touch`
contains mapped-input loads and stores per-thread sums; the joiner consumes them
in the printed checksum. A fresh isolated invocation on the 256 MiB text file
printed checksum **1122379360**. Fault-on-touch map/touch plus unmap was
**34.9 + 13.8 ms** on one thread and **5.5 + 14.8 ms** on eight.
These are one invocation's observations, not a new floor or stable throughput
claim. The probe still touches one byte per page rather than reading every byte.

**Hardware response confirmed.** A freshly built probe passes the checker;
`/bin/true` now fails with 42 missing probes. The original VL and APX functions
contain the intended instruction forms. The signed-overflow reproduction no
longer fails. The full current probe executes and its observed support agrees
with the documented representative forms.

The fresh prefix numbers are **6.68 ticks / 16 elements** versus scalar
**1.69 ticks / element**, about **4.05×**, consistent with the corrected
build-dependent 2–4× account. The old gather working-set, unequal classification
and frequency qualifications are now in the substantive hardware discussion,
not merely contradicted by an appendix. “Every zmm operation costs exactly two
ymm operations” remains broader than the selected kernel experiments justify.
The probe's raw gather output label still says “64 MiB ... random”; the corrected
document's fixed-index working-set qualification must accompany that row.

**Remaining regression-check defect:** `hw/check_probe.py` checks `avxvnni`
with generic `vpdp`. Replacing only that probe with reachable **EVEX/zmm**
`vpdpbusd` still passes, even though the probe is explicitly for **VEX
AVX-VNNI**, a different instruction form. The current unmodified binary uses
the correct VEX form. This is a checker false-positive reproduction, **not**
evidence that the repaired current AVX-VNNI result is wrong.

**Reproduction:** `python3 reviews/audit3-claims/check_claims.py`.
It patches a separate ELF, not `hw/probe.c`. Relevant disassembly and checker
outputs are retained there; actual isolated probe/I/O runs are in
`reviews/audit3-2026-11-10/{hardware,iotest,probe-check}.log`.

## Reproduction record and limits

The machine is the same eight-vCPU EPYC 9554P/KVM environment, GCC 13.3.0.
Default `./test.sh` passes: fixed-point bootstrap, 110 hand cases per stage,
400 compile-fuzz iterations per stage, semantic fuzzing, all pool/path/function
tests and both large compiler comparisons. Bootstrap SHA-256:

```text
b3a84702ef866973b462375aac041f5f805c61a6ba74cd3973bc058cfaf391dd
```

The mutation suite caught all five injected faults; source was restored.
The first-audit deterministic checks still confirm the timer, short-read,
reduced-OpenMP-team and unsigned-prefix fixes. Failure may still leave partial
output, as now explicitly specified.

Correctness work and timing work were separated. Run from the repository root:

```sh
./test.sh
python3 reviews/audit3-carries/probe.py
python3 reviews/audit3-hand/checks.py
python3 reviews/audit3-hand/check_variants.py
python3 reviews/audit3-claims/check_claims.py
python3 reviews/audit3_coverage.py
# Run the following alone, sequentially, with no tests/other jobs running:
python3 reviews/audit3_measure.py
python3 reviews/audit3_controls.py
python3 reviews/audit3_controls.py --only strip,idents,strings,json --rounds 15 --label controls-repeat
python3 reviews/audit_compile.py --reps 15 --output reviews/audit3-2026-11-10/compiler.json
python3 reviews/audit3_ratios.py
```

`reviews/audit3-2026-11-10/README.md` indexes retained evidence and ancillary
probe commands. Generated binaries/large inputs stay under ignored `build/`.
Audit-only scripts redirect paths or build separate controls; no production
implementation change is part of this commit.

The interpreter still shares front-end acceptance logic; compilers share
machine-code stencils; finite tests and a fixed point are not a complete proof.
No research-priority claim or universal statement about all C compilers was
established. Guest quiescence does not eliminate VM/host timing variability.

## What I would investigate next

1. **Adopt the stronger task comparisons before sharpening the headlines.**
   Include packed ordinary-C transitions, retain the faster source form by task
   and resource budget, and test task-specific hand algorithms. Preserve matched
   I/O and both medians and samples; do not select just the favorable batch.
2. **Turn the carry argument into systematic verification.** Cover all ordered
   function pairs, all starting states, all relevant chunk positions and actual
   production boundaries; assert paths. Check combinations with maxima,
   wraparound, local/persistent tees and deferred failures. Independently audit
   the pool-veto assumptions on which the boundary proof depends.
3. **Measure retries, not merely their existence.** Record passes, fallback
   frequency and end-to-end cost by dependency depth, distribution, size and
   thread count. Test whether the pass-count cutoff is a good policy; it is not
   currently a measured equality with serial cost.
4. **Use a precise front-end contract.** Call JSON structural extraction what it
   is; preserve duplicate members in an expanded oracle. Implement and compare
   one real lexer/token stream, including positions and invalid-input behavior,
   before extrapolating the phase table's ratios.
5. **Broaden measurement and provenance.** Multiple fixture distributions/sizes,
   real output consumers, more CPU counts and uncertainty estimates; immutable
   per-row source/binary hashes; refresh/record oneoff inputs and retain its
   samples. Continue toward an independent acceptance oracle and stronger
   instruction-encoding checks.
