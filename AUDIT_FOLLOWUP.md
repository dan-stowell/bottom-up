# Follow-up audit: the response to AUDIT.md

Reviewed **17e317d**, November 10, 2026. This evaluates the changes after the first
audit, not the old implementation. The reproduction scripts and raw observations
are under `reviews/`; no compiler, runtime, or benchmark implementation is changed
by this follow-up.

## Verdict

**The response is substantive, and the main benchmark is much more trustworthy.
It is not yet a blanket validation of the claims.** The new automaton is a real
SIMD transition-function scan, and I found no new successful-execution miscompile.
The table now measures whole cached-file processes, validates each configuration,
checks every timed exit status, and reports interleaved medians and bests.

But there is a new dead-code-elimination bug in the supporting I/O experiment,
another source-form-dependent C baseline, and an overstatement of the new
parallel-carry optimization. The automata results are for a small, explicitly
defined grammar—not a Python lexer—and the hardware document still contains
contradictory conclusions.

## What was actually fixed

| First-audit issue | Follow-up finding |
|---|---|
| Timer accepted failed children | Fixed: `tools/bench.c` rejects failed setup/exec/child exits. |
| Only an unpinned execution was output-checked | Fixed: every timed implementation/CPU configuration is hashed under its actual mask and OpenMP environment. Individual timed executions are status-checked, not rehashed. |
| Best-only, grouped timing | Improved: seven randomized, interleaved rounds, retained samples, median and best wall time. |
| Weak words/pairs/maxgap4 C loops | Rewritten to auto-vectorize; the old leads are withdrawn. |
| C short-read replay and requested-vs-actual OpenMP teams | Fixed in the implementations and deterministic reproductions. |
| Execution probes missing their named instructions | The current binary really contains the intended AVX2/ADX instructions; other affected execution probes were also repaired. |
| Signed scalar-prefix overflow | Changed to unsigned arithmetic; the old UBSan reproduction no longer fails. |
| Failure-output specification | Clarified to permit streamed partial output. This is a specification correction, not transactional output. |
| Compiler's claimed 15% multicore win | Explicitly withdrawn. |

The full `./test.sh` passed: fixed-point bootstrap, **110 hand cases per stage**,
400 compile-fuzz inputs and semantic fuzz per stage, **3,968 pool runs over 32
programs**, and both large-input checks. Additional targeted tests use
non-forgetting permutation automata, all eight stack depths, boundary lengths,
and dependent carries, comparing serial and parallel binaries with the scalar
semantic interpreter (which shares front-end acceptance logic). All 26 targeted
cases passed in serial, four-CPU, and four-CPU eager modes. These are evidence of
correctness, not a proof of all programs.

## Fresh reproduction of the main table

I reran all 21 programs with fresh **256 MiB** fixtures, GCC **13.3.0**, CPUs
`[3]`, `[0,1,2,3]`, and `[0,1,2,3,4,5,6,7]`, and seven interleaved rounds.
Every configuration's output check passed. Selected median throughput ratios
against the supplied “best C” column:

| Task | 1 CPU | 4 CPUs | 8 CPUs |
|---|---:|---:|---:|
| words | 1.10× | 1.03× | 0.98× |
| letters filter | 3.94× | 2.23× | 2.20× |
| dword filter | 1.64× | 1.75× | 1.45× |
| strip | 3.13× | 5.01× | 4.12× |
| csv | 4.07× | 1.90× | 1.59× |
| idents | 4.19× | 3.01× | 3.18× |
| strings | 3.55× | 6.87× | 3.66× |

**The qualitative result reproduces:** the old word-count lead is gone; filters
and the specified automata retain substantial advantages over these C
implementations. Exact range endpoints are not constants. Multicore samples
vary considerably; CSV on eight CPUs was 1.59× here, not 2×, while another row
exceeded the advertised range.

Small-program compilation latency also reproduces: for `words`/`strip`,
21 grouped, one-CPU runs gave Strand medians **0.208/0.232 ms**, versus
**63.8/86.7 ms** for `gcc -O3 -march=native -fopenmp` compiling `base_omp.c`.
These are whole compilation commands: Strand emits an ELF to `/dev/null`;
GCC compiles and links an executable file. They are not compiler-internal
phase timings. The unoptimized OpenMP C builds took 38.9/42.0 ms.

The timing boundary is appropriate **if the intended task is running an already
compiled program over a warm cached file and discarding output**. It includes
process startup, mapping/reading, faults, computation, output syscalls, and exit.
It does not measure storage throughput, delivery to a real output consumer,
compute-only SIMD throughput, or compilation plus execution.

The supplied C implementations are legitimate comparators. They are not an upper
bound on C, nor a test of every compiler. In particular, “no C compiler vectorises
or threads” an automaton is stronger than this GCC experiment establishes. The
repository's own faster intrinsic-based C automaton prototype is disclosed and
uses a different timing boundary; it must not be compared directly with the
whole-process table.

## Remaining findings

### 1. The I/O race fix removed the actual page touches

**Measurement defect, `explore/iotest.c:13–16`.** Per-thread `sums[16]` removed the
shared-update race, but nothing ever consumes those sums. With the documented
`gcc -O2 -pthread`, GCC removes every load from the mapped input in `touch`.
The resulting function loads `per` and executes an empty integer loop; it does
not reference `base` or store a checksum.

Consequently **“mmap, fault on touch” no longer faults on or reads the file**.
`MAP_POPULATE` still prefaults in the mapping syscall, and `pread` still performs
its reads: this does not invalidate every row, or the separate 21-program table.

One invocation on the same 256 MiB cached file produced the following
fault-on-touch row, in milliseconds (**map + touch**, followed by unmap):

| Version | 1 thread | 8 threads |
|---|---:|---:|
| Current source | 0.3 + 0.0 | 1.9 + 0.0 |
| Observable checksum | 23.5 + 16.4 | 5.5 + 10.4 |

These single-run numbers illustrate the missing work; they are not a new
stable throughput or lower-bound claim. The disassembly establishes the defect
independently of timing.

The scratch correction combines the per-thread sums into an observable sink
after joining, without shared worker updates. Its disassembly restores the input
loads. A permanent fix should consume/print the checksum and verify the optimized
assembly. Even a repaired one-byte-per-page experiment is not a universal I/O
lower bound; the revised documentation correctly withdrew that interpretation.

### 2. Swapcase still confuses a source shape with the strongest task baseline

`bench/base_omp.c:103–113` constructs an ASCII case-swap table arithmetically,
then forces each byte through `out[i] = tab[c]`. Replacing just that assignment
with this ordinary C expression auto-vectorizes under the **same compiler flags**:

```c
int u = c & 223;
out[i] = c ^ (((u >= 65) & (u <= 90)) * 32);
```

No intrinsics, I/O changes, partitioning changes, or extra threads are involved.
The outputs agree exhaustively for all 256 byte values and on the full timed
input under every CPU configuration.

Nine randomly interleaved trials per configuration, on the same 256 MiB input,
gave these **median GB/s**:

| CPUs | C table | C arithmetic | Strand | Strand / C arithmetic |
|---:|---:|---:|---:|---:|
| 1 | 2.37 | 4.93 | 5.49 | 1.11× |
| 4 | 5.65 | 7.39 | 12.04 | 1.63× |
| 8 | 6.09 | 8.60 | 9.90 | 1.15× |

The gap shrinks substantially, but does **not** disappear at every CPU count
in this batch. Multicore bests differ markedly from medians; all samples are
retained rather than selecting the most favorable statistic.

This does **not** show that GCC can vectorize arbitrary byte-table translation.
It shows that this particular lookup computes a simple arithmetic function.
Keep a table-shaped baseline if the claim is specifically about that primitive;
add a genuinely arbitrary table fixture. Include the arithmetic version before
calling the comparison “strongest C” for the **swapcase task**.

### 3. One-chunk warm-up does not make every dependent `prev` parallel

`SWEETSPOT.md:156–161` says warm-up makes dependent `prev` carries exact.
A concrete exception is:

```text
input 1 & scan dup prev dup prev + + out4
```

On a 524,289-byte file of `0x01`, it enters the serial-fallback path on
2, 3, 4, and 8 allowed CPUs. This is an **execution-path observation**, not an
inference from noisy timings: a scratch copy of the generated ELF has only the
`fsbad` entry replaced by `exit_group(77)`. Controls with a single `prev`, a
permutation DFA, and DFA followed by one `prev` do not take that path.
Unmodified executions produce the correct scalar-interpreter output.

The fallback is a correctness safeguard working as intended, not a miscompile.
But it narrows both the new optimization claim and “every line can be split
across cores” (`README.md:14–17`, `SWEETSPOT.md:27–32`). Each primitive's
associativity is not sufficient to guarantee that the current compact summary
works for arbitrary compositions. Persistent-tee rereads, memory compression,
and scatter also have explicit pool vetoes.

This detector establishes that fallback occurs; it does not establish whether
this particular case needs a better carry summary or a less conservative
comparison of carry state. Distinguishing those is a useful follow-up.

`tests/pool.py` already includes chained `prev`, but checks output equivalence,
not whether parallel execution succeeds. Add an execution-path assertion or
instrumentation if the test is meant to substantiate the optimization.

### 4. The source-text automata use a toy grammar

The Python standard library is a useful real-source **byte distribution**, but
the benchmark DFA only models its stated double-quote/hash/backslash grammar.
The “idents” task counts `[A-Za-z0-9_]` runs, including digit-starting runs.
Both implementations agree on these examples:

| Program and input | Benchmark result | Python lexical interpretation |
|---|---|---|
| `strip`: `x = '# not a comment'\n` | `x = '\n` | The quoted text is a string, not a comment. |
| `strings`: `'abc'\n` | 0 | One string token. |
| `idents`: `123 abc\n` | 2 | One number and one identifier. |

These are **not Strand compiler bugs**. The simplified mechanics are described
in `SWEETSPOT.md`, and the measurements remain useful for that grammar.
The broader “source text with string awareness”/“lexing” headlines should say
“simplified four-state grammar,” rather than imply correct Python lexical work.
Agreement between two implementations of the same toy DFA does not establish
correctness for a different language.

### 5. Hardware verification and conclusions need another pass

- `python3 hw/check_probe.py /bin/true` prints **`probe check: ok`** and exits 0.
  It checks only matching symbols it finds, never that all expected probes exist.
  Its APX and AVX-512VL patterns are also too weak to verify those extensions.
  This is a regression-checker gap, **not** evidence that the current repaired
  AVX2/ADX probes are still wrong.
- `HARDWARE.md:76–78` says the unsigned prefix correction's figure “did not move,”
  while quoting SIMD **6.7 ticks per 16 elements** versus scalar **1.7 per element**.
  Those imply about **4.1×**, not the old 2× retained in the table and conclusion.
  The follow-up probe measured 6.73/vector versus 1.69/element, also about 4×.
- The appendix correctly qualifies gather working set, unequal classification
  tasks, and the frequency experiment. Yet the old table still labels the gather
  “64 MiB ... random ... memory-bound,” and the conclusion still asserts internal
  gather/scatter serialization. The corrected interpretation must replace those
  claims, not coexist with contradictory unmarked headlines.

## Reproducibility details still worth improving

`bench/run.py:24–30` reuses fixtures solely by filename, despite changing their
generators. Existing users can silently benchmark old data while reading the new
distribution description. All four existing fixtures on this VM matched the
freshly generated fixtures' hashes; there is no evidence that the published
replacement table used stale data. The follow-up avoids the general cache
problem by creating a fresh directory.
Record input hashes, generator/source revision, toolchain, and actual masks.

“Best C” at 4/all CPUs means the OpenMP implementation at exactly that thread
count; at one CPU it considers both C implementations. That policy is disclosed
as “same CPU count,” but it is not “fastest C using at most this resource budget.”
Keep the distinction explicit, especially when fewer threads win.

## Reproduce

From the repository root:

```sh
./test.sh
python3 reviews/followup_measure.py
python3 reviews/followup_checks.py
python3 reviews/audit_checks.py --output build/reaudit/old-checks.json
```

Run timing campaigns without concurrent tests or other benchmarks.
`followup_measure.py` copies `bench/run.py` and changes only its path assignment
to generate fresh fixtures; it also records the matched arithmetic-swapcase
comparison, original/observable I/O probes, hardware checks, and small-program
compilation latency. `followup_checks.py` preserves targeted correctness checks,
the fallback detector, and the grammar examples. Generated inputs and binaries
stay under ignored `build/`.

Retained evidence: `reviews/reaudit-2026-11-10/README.md` indexes the full table,
all timing samples, fixture hashes/environment, test logs, deterministic
observations, and original/corrected I/O disassembly. The scratch I/O correction
and arithmetic C source are included as experimental controls, not installed fixes.
The prioritized investigation backlog is at the top of `AUDIT.md`.
