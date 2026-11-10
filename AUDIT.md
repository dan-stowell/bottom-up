# Correctness and measurement audit — November 10, 2026

## Follow-up verdict after the audit response

**Reviewed updated revision `17e317d`.** The original audit below concerns
`ca01f9d`; do not read its already-fixed defects as findings about the current
implementation. Full follow-up measurements and counterexamples are in
`AUDIT_FOLLOWUP.md`, with evidence under `reviews/reaudit-2026-11-10/`.

**There are important qualifications to the performance story, but no finding
that invalidates the working compiler or the transition-function-scan approach.**
The response fixes most original defects. The full updated test suite passes,
including 3,968 pool runs; additional targeted automata/carry tests pass. All
21 benchmarks passed their per-configuration output checks on freshly generated
256 MiB inputs. The specified automata still beat the supplied C implementations
by roughly **3.1–4.2× on one CPU**. Those gains are real for these implementations
and tasks—not proof of superiority over the best possible C algorithm.

### Findings that materially change the interpretation

1. **“Strongest C” remains too strong.** Swapcase's table-based C loop can be
   replaced by ordinary arithmetic that auto-vectorizes, with no intrinsics or
   I/O changes. Strand's measured advantage over that version is
   **1.11× / 1.63× / 1.15× on 1 / 4 / 8 CPUs**, rather than a general approximately
   2× task advantage. This repeats the earlier distinction between beating a
   particular source form and beating a task's strong implementation. It does
   not disprove an advantage for genuinely arbitrary table translation.
2. **Automatic parallelism is conditional, not guaranteed for every line.**
   `input 1 & scan dup prev dup prev + + out4` still falls back to serial on
   2/3/4/8 CPUs for a simple all-ones fixture. Direct execution-path
   instrumentation confirms this; unmodified output is correct. The new
   runtime still does not parallelize every dependent-carry pattern, and existing
   tests can pass while the advertised optimization falls back. This qualifies
   the programming-model/performance promise, not the semantic correctness result.
3. **A supporting measurement is genuinely broken.** The I/O-probe race fix
   leaves per-thread checksums unconsumed. GCC deletes all mapped-input loads
   from its “fault-on-touch” worker. That row no longer measures the claimed
   work. This invalidates that experiment, **not the separate main benchmark
   table**. The hardware checker also falsely accepts `/bin/true`, and several
   hardware conclusions still contradict their correction appendix.
4. **The practical automata scope needs explicit limits.** The source fixture
   is Python code, but the benchmark recognizes a simplified four-state grammar,
   not Python syntax. For example, it treats the `#` inside a single-quoted
   string as a comment. C and Strand agree on the toy grammar; this is not a
   compiler bug. The gains do not establish correct general-purpose source
   lexing at those speeds.

**Bottom line:** retain the compiler's validity and the measured advantages on
the defined filter/automaton workloads. Narrow universal claims about C,
source-spelling independence, parallelization, and real-language lexing.
The most important unresolved question is the size and cause of the advantage
against stronger *task-level* alternatives, not whether the project works at all.

### Further investigations worth carrying through

These are proposed follow-ups, **not completed results**:

- **Highest value: strengthen the task comparisons.** Include arithmetic
  swapcase in both C baselines, a genuinely arbitrary byte-table fixture, and
  alternative automata/CSV algorithms. Compare the repository's intrinsic C
  prototype with Strand under identical whole-process I/O and threading.
  Separate automatic code-generation convenience from algorithm, layout, and
  runtime gains; do not assume the remaining gap will survive or disappear.
- **Make successful parallel execution observable.** Add a test-only fallback
  counter/path assertion; test chained carries, non-forgetting automata, and
  boundary lengths. Measure fallback frequency and its performance cost, not
  just correct final output. Distinguish insufficient carry summaries from
  unnecessarily strict comparisons of carry state.
- **Broaden correctness and task coverage.** Exercise all 256 possible
  four-state transition functions and their pair compositions, plus randomized
  cross-chunk/thread sequences. For any real-language lexer claim, define the
  grammar and compare with an independent lexer—not just matching toy C code.
- **Measure robustness and practical end-to-end use.** Vary input size,
  distributions, thread counts, and output destinations; repeat timing batches
  with uncertainty estimates. Include compile-plus-run latency for one-off
  queries and record fixture/source/toolchain hashes.
- **Repair, then remeasure the supporting probes.** Consume the I/O checksums;
  make the instruction checker reject missing probes and wrong instruction
  families; replace contradictory hardware headlines with the qualified
  conclusions. These defects are already confirmed and do not require more
  investigation before fixing.

---

## Original audit: revision ca01f9d

Reviewed revision: `ca01f9d07d5bdf19d7767113740ee258e0c0ee3c`.
File/line references below refer to that revision unless stated otherwise.
Experiments used the project's existing VM: EPYC 9554P under KVM, eight allowed
vCPUs (0–7), GCC 13.3.0, binutils 2.42, Python 3.12.3.

## Verdict

**The working self-hosted compiler is real, and most reported throughput numbers
broadly reproduce. The stronger performance explanations do not all stand up.**

- The default full test suite passes, including bootstrap equality, all three
  stages, compiler/semantic differential tests, and 2,728 thread-pool runs.
  I found no new counterexample where a successfully executing Strand program
  produces the wrong result.
- The 17-program benchmark really times whole processes over cached input. Its
  single-CPU results are close to the published table; multicore results vary
  considerably. Those numbers are **not isolated compiler/SIMD speedups**.
- The assertion that word counting requires handwritten C intrinsics is false:
  a small boundary-peeling rewrite makes GCC vectorize the existing OpenMP C
  baseline and largely eliminates Strand's claimed advantage on that workload.
  Filtering still shows a substantial advantage over the supplied C baseline.
- There are confirmed defects in the timing harness, C baselines, and hardware
  probes. These need fixing before treating the project as a reliable measurement
  suite. They do not establish that every existing number is wrong.
- “No loops/branches” is defensible as a **source-language restriction**, not as
  a description of the machine code or runtime. The documentation mostly admits
  this, but some headlines and older sections remain too broad.

The compiler/runtime were not changed by this audit. The changes are this report,
reproduction scripts, retained evidence, and a deterministic `E_RACE` hand case.

## What reproduced

### Bootstrap and tests

`./test.sh` with its default 400 fuzz iterations completed successfully.
It builds from the checked-in sources rather than relying on existing binaries.
There were 96 hand cases per stage at the reviewed revision, plus both large-source
comparisons. A separate C self-compilation also matched the stage binaries.

All three bootstrap stages have SHA-256:

```
8ff94a8bf940ef259812831096a42bec69a90ee8308f658527d229e7eeb07428
```

The current compiler source including tables is 81,130 bytes; the executable is
187,950 bytes. The `2736fc84…`, 44.7-KB source, and 117,000-byte executable in older
report sections describe an earlier revision, not the current build.

The mutation suite caught all five injected bugs. After adding the `E_RACE`
fixture described below, all three stages passed **97 hand cases**.

A fresh, single-CPU ptrace self-compilation counted **8,709,660 instructions**:
59.5% ordinary vector instructions, 29.5% GPR bookkeeping, 8.6% mask operations,
1.7% gathers, and 0.7% scatters, using the project's classifier. This supports
“SIMD throughout the compiler's phases,” not “100% SIMD.”
It is a current measurement, not a reproduction of the old 61.5% figure.
The profiler does not trace worker clones; pinning to one CPU avoids that issue.

This is strong testing evidence, **not a proof**:

- All three code generators use the same generated machine-code stencils and
  class tables. Agreement cannot detect a shared bad stencil.
- The interpreter independently models operations, but calls the Python
  compiler's `analyze()` (`boot/strand.py:280`). It is not an independent oracle
  for lexing, acceptance, or diagnostic priority.
- Stage equality proves a fixed point on the compiler's own source, not equality
  of compiler functions on every possible input. Repeating the same seeds on
  byte-identical stages is useful bootstrap checking, not independent coverage.
- The pool tests compare large executions to the same binary run serially;
  the independent interpreter is used only for inputs up to 4,097 bytes.
  Large pipe/eager combinations are deliberately skipped
  (`tests/pool.py:61–68`). Resource limits and `E_COLLISION` remain untested.

### Published streaming table

`python3 bench/run.py 256` passed its output comparisons for all 17 programs.
Below are representative **published → reproduced** results in decimal GB/s;
the complete rerun is in `reviews/audit-2026-11-10/suite-256.log`.

| Program | GCC -O3 | Strand, 1 CPU | Strand, 8 vCPUs |
|---|---:|---:|---:|
| lines | 5.98 → 5.94 | 8.23 → 8.10 | 14.46 → 14.67 |
| words | 1.16 → 1.16 | 7.77 → 7.74 | 16.15 → 12.69 |
| pairs | 1.15 → 1.15 | 7.48 → 7.49 | 17.46 → 15.38 |
| upper | 6.63 → 6.14 | 6.08 → 5.99 | 12.10 → 10.66 |
| letters | 1.40 → 1.39 | 6.41 → 6.26 | 14.39 → 13.31 |
| maxgap4 | 3.98 → 4.13 | 8.78 → 9.49 | 19.42 → 18.21 |
| prefix4 | 4.96 → 5.20 | 6.43 → 6.76 | 13.09 → 14.31 |

The workload/metric description is substantially accurate: process startup,
input mapping/reading and faults, computation, output syscalls, and process exit
are included; output goes to `/dev/null`. This does **not** measure disk
throughput, useful delivery of output to another consumer, compilation time,
or a compute-only kernel.

The scalar C column intentionally disables vectorization; the GCC column enables
it. Both use a single thread and 64-KiB reads, whereas Strand maps regular files
and may use several CPUs. The documentation's caveat about combining SIMD,
threading, and I/O advantages is important and correct.

The data coverage is narrow: pseudo-text contains only lowercase ASCII, spaces
and newlines, so `nonascii` always reduces to zero. The integer file repeats a
1-MiB Gaussian-data block. Output equality on these fixtures is valuable, but
does not cover arbitrary byte distributions, short reads, or extreme integers.

### Matched C comparison: the main claim that fails

`bench/base_omp.c:26–29` puts the first-element special case inside every iteration:

```c
for (size_t i = 0; i < n; i++)
  acc += (in[i] > 32) & (i == 0 || in[i - 1] <= 32);
```

Replace that with:

```c
acc = n && in[0] > 32;
#pragma omp parallel for reduction(+:acc) schedule(static)
for (size_t i = 1; i < n; i++)
  acc += (in[i] > 32) & (in[i - 1] <= 32);
```

With **the same** `-O3 -march=native -fopenmp`, GCC reports 64-byte vectorization
(plus a 32-byte tail). No intrinsics, special SIMD pragma, or new algorithm is
needed. A similar adjacent-load rewrite also vectorizes the ordinary read-based
`words` and `pairs` loops.

I ran a matched comparison with 256-MiB cached files, the same CPU masks, explicit
OpenMP thread counts, checked child statuses, output hashes checked in **every**
timed configuration, and nine randomly interleaved trials per configuration.
Values are **best / median GB/s**:

| CPUs | Original OpenMP words | Peeled OpenMP words | Strand words |
|---:|---:|---:|---:|
| 1 | 1.55 / 1.53 | 6.98 / 6.36 | 7.83 / 6.51 |
| 4 | 5.25 / 3.91 | 16.22 / 10.59 | 18.00 / 12.70 |
| 8 | 6.60 / 4.72 | 14.87 / 12.19 | 16.12 / 10.58 |

This changes the best-time advantage over C to approximately **1.1x**, not
3–4x. The medians also show why the small remaining differences should not be
oversold on this VM.

This directly contradicts `SWEETSPOT.md:139–150`: needing the previous element
does not, by itself, prevent this GCC from vectorizing or require handwritten
intrinsics. The measured original loops really are slow; the explanation and
generality are the problem.

Do not generalize that correction to every workload: for `letters`, the matched
best results were C/Strand **1.50/5.83**, **5.02/13.38**, and **5.75/13.80** GB/s
on 1/4/8 CPUs. A useful filter advantage remains against this baseline.
There is still no matched comparison to hand-vectorized parallel C filters.

### Compiler speed

An initial checked, 15-trial reproduction gave:

| Source | C, 1 CPU | Strand, 1 CPU | C, allowed 8 | Strand, allowed 8 |
|---|---:|---:|---:|---:|
| 81,130 bytes, itself | 3.028 ms | 4.498 ms | 3.228 ms | 4.863 ms |
| 5,252,294 bytes, 64 copies | 231.085 ms | 345.367 ms | 213.852 ms | 248.095 ms |

These are minima, using the same checked timing boundary as the main suite.
The small-input slowdown reproduces. The reported **15% large-input win does
not reproduce in this batch**; parallel Strand is faster than serial Strand,
but not faster than C here.

A second run with **21 randomly interleaved trials** and output hashes checked
for each source/compiler/CPU-mask combination gave the following large-input
**best / median** times:

| Allowed CPUs | C | Strand |
|---:|---:|---:|
| 1 | 250.375 / 259.466 ms | 355.282 / 367.625 ms |
| 8 | 233.941 / 259.843 ms | 256.616 / 301.526 ms |

Thus the claimed win did not reproduce in either batch. This does not prove the
earlier run was impossible; it shows that its small advantage is not robust
enough to present as an established result on this VM.

## Confirmed defects and corrective actions

### 1. Failed programs can be reported as successful fast benchmarks

**High importance for measurement validity.**
`tools/bench.c:14–17` ignores `execv()` failure and the child's exit/signal status.
`bench/run.py:25` also ignores the timer's status. For example:

```
build/suite/bench 1 /dev/null /bin/false
```

prints timings and exits **0**, even though its child exits 1. A crashed or
otherwise failed timed binary can therefore appear to process the full file
extremely quickly.

`run.py:42` hashes one unpinned execution of each binary; it does **not** validate
the separate 1/4/8-CPU paths subsequently timed at line 45. The claim in its
docstring that all configurations' outputs are checked is too strong.
Our separate matched runner checked every configuration and found no such
failure in those runs; this is a harness vulnerability, not evidence that the
published table was fabricated or consists of failed runs.

**Fix:** reject failed opens/forks/execs/waits and nonzero/signal child statuses;
check outputs under the actual affinity/environment; retain per-trial samples.
Also avoid using independently sorted minimum user/system times and the *last*
iteration's fault count as if they described one execution.

### 2. The C baselines have real edge-case correctness bugs

**Medium; the default regular-file fixtures hide them.**

- `bench/base.c:75`: `#if K >= P_sum4` also selects byte kernels `swapcase=16`
  and `lines32=17`. It incorrectly carries 1–3 residual bytes between short reads,
  processing them again. With legal one-byte reads forced by an interposer,
  swapcase on `aB` produces **`AAb` instead of `Ab`**; two newlines count as
  **3 instead of 2**. Restrict remainder handling to the actual dword kernels.
- `bench/base_omp.c:38–60` partitions using `omp_get_max_threads()` and assumes
  that many workers actually run. With `OMP_NUM_THREADS=4 OMP_THREAD_LIMIT=1`,
  letters on `abcd` produces **`a`**, and prefix sum on `[1,2,3,4]` produces
  **`[1,0,0,0]`** instead of `[1,3,6,10]`.
  Partition by the actual team size inside the parallel region.

The reviewed 256-MiB ordinary-file runs do not exhibit these failures.
The new matched run fixes `OMP_DYNAMIC=FALSE` and records the requested sizes;
its output checks would catch reduced-team corruption.

### 3. Some hardware probes do not measure what their labels claim

**High importance for hardware conclusions; not evidence of a bad Strand binary.**

- **False execution tests:** `hw/probe.c:34`'s AVX2 test compiles to scalar
  `add eax,eax`; line 40's ADX test uses ordinary `add`/`adc`, not `adcx`/`adox`.
  These “executes” results do not validate those extensions. The volatile source
  prevents constant folding of the input, but does not force use of the intended
  instruction. This survives the earlier documented fix for the FP16 probe.
  Keep the intended instruction observable and mechanically check disassembly.
- **Undefined scalar baseline:** `hw/probe.c:125` accumulates random positive
  values in signed `int32_t`. UBSan immediately reports signed overflow on the
  actual input distribution. Use defined unsigned wrapping arithmetic for the
  SIMD/scalar prefix-sum comparison.
- **Mislabelled gather working set:** line 119 uses the same 1,024 indices on
  every repetition, each generated from `B[i] % 1024` then shifted/scaled.
  At most 1,024 cache lines are touched, at addresses below 16 MiB, despite the
  64-MiB allocation. This is not a random traversal of a 64-MiB data working set;
  the “memory-bound” interpretation in `HARDWARE.md:51` is not demonstrated.
- **Non-equivalent classification kernels:** lines 129–135 compare random
  128-entry byte tables with a different dword lookup and four times the output
  width. The roughly 40x ratio describes those particular representations and
  kernels, not a verified 40x speedup of the same compiler classification task.

Many raw kernel timings nevertheless reproduce closely: zmm/ymm add
**1.92/0.90** ticks per vector, dword compress register/memory **4.45/72.05**,
L1 gather **18.38**, and SIMD prefix **6.64**. Some do not: byte register-compress
was **8.72** rather than 4.9, and scatter **27.55** rather than 20.8.

A load/operation/store microbenchmark is not a standalone instruction latency
measurement. Equal total cost to scalar code does not prove every gather lane
is internally serialized. Likewise a scalar spin *after* vector heating, with
one before/after comparison, is insufficient to prove “no AVX-512 frequency
penalty” in general. Report these as observations about these kernels on this VM.

### 4. The “I/O floor” is an experiment, not a proved lower bound

`explore/iotest.c:14–15` has concurrent non-atomic `sink += s` updates; `volatile`
does not make them race-free. It should collect per-thread results and combine
them after joining. The timings also include thread creation/join, and the mmap
path touches one byte per page while `pread` copies the full input.

My single rerun's fault-on-touch mmap totals, including unmap, ranged from
**44.7 ms on one thread to 17.7 ms on eight**. The numbers vary with strategy
and scheduling. These measurements support “mapping/faulting/teardown can be
expensive,” not “anything near 13–21 GB/s measures only the kernel.”
`SWEETSPOT.md:105–110,136–140` overstates that inference.

The separate ceiling probe broadly reproduces too. Its current documented build
retains the timed calls; I did **not** find dead-code elimination invalidating
that table. Its threaded word counts deliberately approximate partition seams,
as acknowledged in the source: it is a throughput probe, not a correctness oracle.

### 5. Failure-output semantics contradict the language description

`DESIGN.md:63` promises `halt` exits with “no output,” and the interpreter returns
empty output on halt/failure. But:

```
~300000 out
1 halt
```

exits 1 after writing **262,144 bytes**, even on one CPU without eager mode.
The output buffer has already flushed. `tests/cases.py` and `tests/fuzz_sem.py`
deliberately ignore stdout on any nonzero status.

**This is not a newly discovered runtime regression:** `REVIEWS.md`, review 3,
already declares failure output unspecified. The problem is that the public
language description and “interpreter agreement” claim still imply a stronger
contract. Make the chosen streaming semantics authoritative everywhere, or pay
the cost of transactional output. Tests currently establish status agreement,
not complete observable equivalence on failure.

### 6. Portability and rejection coverage need tightening

- README's CPU prerequisites omit **AVX-512 VBMI and VBMI2**, required by the
  byte-table `vpermi2b` and byte-filter `vpcompressb` stencils
  (`tools/mkstencils.py:335–338,376`). List those requirements or add dispatch
  and fallbacks. This is a source/instruction audit, not a run on a restricted CPU.
- `bench/run.py:23,44–45` and `tests/pool.py:41–43` turn the *count* of allowed
  CPUs into hard-coded IDs starting at zero, and assume CPU 3/four CPUs exist.
  On a restricted cpuset they can fail or benchmark a different affinity than
  intended. Use the actual `sched_getaffinity()` IDs.
- A four-round race rejection is easy to construct, independently of any
  64-bit fingerprint collision. A 137-byte, 16-name program rejects with
  `E_RACE=4` identically in Python, C, and all bootstrap stages.
  It took 2,835 candidate hashes to construct a tree of later colliding blockers.
  This is **allowed by the spec**, not a miscompile. It does refute the practical
  implication that the missing error-4 fixture requires an infeasible collision
  search, and the `2^-64` discussion must not be applied to race exhaustion.
  The fixture is now in `tests/cases.py`; its generator is in `audit_checks.py`.
- Older text needs explicit versioning: current byte lines are not i32 lines;
  the old 4-GiB cumulative output limit is not enforced by the streaming runtime;
  the precise input cutoff is `0x7fe00000`, not all sizes below 2 GiB;
  the ledger's claim that only eager fuzzing covers the runtime predates
  `tests/pool.py`.

## Reproduce this audit

Run these sequentially, not alongside other benchmark loads:

```sh
./test.sh
tests/mutation.sh
python3 bench/run.py 256
python3 reviews/audit_bench.py --output build/review/matched.json
python3 reviews/audit_compile.py --output build/review/compiler-randomized.json
python3 reviews/audit_checks.py --output build/review/checks.json
```

The last script records known bad observations rather than treating them as
desired regression-test behavior. The matched scripts retain every timing,
check every timed configuration's outputs, and reject failed children.
They are audit tools, not replacements for fixing the production benchmark.

Additional probes:

```sh
gcc -O2 -o build/review/probe hw/probe.c
taskset -c 2 build/review/probe
gcc -O2 -pthread -o build/review/ceiling explore/ceiling.c
build/review/ceiling 256
gcc -O2 -pthread -o build/review/iotest explore/iotest.c
build/review/iotest build/suite/text256
gcc -O2 -o build/review/prof tools/prof.c
taskset -c 3 build/review/prof count build/strandc.strand build/stage2 > build/review/counts
python3 tools/profreport.py build/strandc.strand build/stage2 build/review/counts
```

Raw logs, environment and input hashes, per-trial JSON, observed counterexamples,
and GCC's vectorization report are in `reviews/audit-2026-11-10/`.

**Recommended positioning:** a functioning, tightly constrained self-hosted
array-language compiler with compact SIMD streaming programs and useful filter
performance. Not a proof that adjacent-element C loops require intrinsics,
that all work is SIMD, or that the hardware explanations and speedups generalize
beyond these implementations and this VM. Novelty/priority in the research
literature was not established by this code-and-experiment audit.
