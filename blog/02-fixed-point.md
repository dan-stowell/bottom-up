# Fixed point

*Checkpoint 2: the compiler compiles itself. 2026-11-10.*

```
$ ./build.sh
stage1 == stage2 (SIMD compiler reproduces the scalar reference)
stage2 == stage3 (fixed point)
7fe5d2ff83dde243f00bb78266dfbf7a5470308c4540861a259ca94804548fe8  build/stage1
7fe5d2ff83dde243f00bb78266dfbf7a5470308c4540861a259ca94804548fe8  build/stage2
7fe5d2ff83dde243f00bb78266dfbf7a5470308c4540861a259ca94804548fe8  build/stage3
```

- **stage 0**: the scalar reference compiler in Python.
- **stage 1** = stage0(compiler.strand): a SIMD binary produced by scalar code.
- **stage 2** = stage1(compiler.strand): a SIMD binary produced by SIMD code.
- **stage 3** = stage2(compiler.strand).

stage2 == stage3 is the fixed point. stage1 == stage2 is the stronger statement: the
SIMD compiler and the scalar reference are the same function on this input. The first
time I ran the bootstrap it failed once (a gather index of −1 on the sigil byte of `~1`,
caught by the runtime bounds check), and on the second run all three hashes matched.

## Why it was that easy

Because the hard part had been moved. The Strand compiler was written *against a
byte-exact scalar specification*, with a differential fuzzer already running. By the time
it could compile `input out`, it could compile itself; there was no separate "now make it
self-host" phase. A language with one type, no control flow and no nesting has very few
places for a self-hosting bug to hide.

The fixed point has since survived three redesigns of the code layout (memory release,
prologue hoisting, placeholder-based patching). Each changed every byte of the output and
each re-converged. (*Correction: I first wrote "on the first try". The release redesign
failed once with a runtime bounds check before converging. What is true is that no mistake
has ever shown up as stage2 != stage3; they are caught earlier.*)

## What a fixed point does not prove

It proves the compiler is a consistent function of its own source. A compiler that
miscompiles `+` as `-` everywhere can reach a fixed point too. What argues correctness
here is separate, and runs on every stage (`./test.sh`):

1. **Semantics.** Random Strand programs → compiled binary vs. a scalar interpreter that
   shares no code generation with anything. Inputs are sized to straddle 16-lane chunks.
2. **Bytes.** The stage under test vs. two scalar compilers (Python, C; *correction: when
   this was first posted the suite only ran the Python one — Codex caught that, see
   REVIEWS.md — the C oracle was wired in afterwards*) on random valid
   programs, on the same programs shifted so tokens land at every offset mod 16, and on
   mutated malformed programs, where the exit status must be the same lowest-numbered
   failing check.
3. **58 hand-written cases**, one or more per compile error.

All three stages are the same bytes, so "run on every stage" currently means running the
same binary three times. It is still wired that way, because the day stage1 ≠ stage2 the
suite needs to say which one is wrong.

## Is the compiler really an output of its own SIMD code generator?

Counting executed instructions of stage2 compiling itself (ptrace single-step, exact):

| | instructions | share |
|---|---|---|
| vector (zmm/ymm/xmm) | 2,753,634 | 55.0% |
| scalar bookkeeping: loop control, pointer loads, checks, allocator | 1,849,927 | 36.9% |
| mask-register ops | 274,216 | 5.5% |
| gather (16 serial loads each) | 119,712 | 2.4% |
| scatter (16 serial stores each) | 12,378 | 0.2% |
| startup/exit stub (syscalls) | 100 | 0.002% |

Every one of those except the last row was emitted by the stencil gather in
`src/compiler.strand`. There is no runtime library: scans, compress-stores, gathers and
the allocator are all stencils inside the per-line loop. The 100 stub instructions are
the hand-written island (mmap, read, write, exit).

Where the claim is weaker than it sounds is recorded in SCALAR_LEDGER.md: the stencil
*bytes* were assembled by GNU as, not by Strand; gather/scatter are serial inside the CPU;
and the scalar C compiler is still 2.5× faster end to end.

## Update, same day: what the review broke

Codex's adversarial review (REVIEWS.md, review 2) found that "the same function as the
scalar reference" was false on inputs my fuzzers never produced: two names with the same
32-bit hash made the SIMD compiler reject a program the reference accepted, and so did
40,000 distinct names. It also found that `build.sh` could not fail (`cmp && echo` under
`set -e`), so the fixed-point check above was decorative. Both are fixed; the name race is
now part of the language definition and both scalar references simulate it. The compiler
has been rewritten around fused loops since, and the fixed point is now `2736fc84…`, still
with stage1 == stage2 == stage3. A fixed point really does not prove correctness — a reviewer with a different
imagination found in minutes what thousands of fuzz cases had not.
