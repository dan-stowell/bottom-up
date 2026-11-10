# REPORT.md

**Strand** is a language with no loops, no branches and no functions, whose compiler —
149 lines of Strand — runs as AVX-512 code in every phase and compiles itself to a fixed
point: stage1 == stage2 == stage3, sha256 `2736fc84…`, where stage1 was built by a scalar
Python reference and stage2 by the SIMD compiler.

The short version of the result:

- **It works and it is self-hosting.** The fixed point was reached on the second bootstrap
  attempt and has re-converged after each of nine redesigns of the output. Every mistake on
  the way was caught earlier — by the stage-0 compiler or by a runtime bounds check — and
  never showed up as stage2 != stage3.
- **It is checked against three independent things**: a scalar interpreter, a scalar
  Python compiler and a scalar C compiler, byte for byte, including malformed input.
- **It is not faster than scalar code.** The SIMD compiler is 1.4-1.9x *slower* than the
  obvious C implementation. It started 2.5-3.9x slower. The reason is measured and is not
  what I expected.

## What is genuinely new here (as far as I can tell), and what is borrowed

Borrowed, and said so: array-style compilation (Co-dfns, Pareas); scan-based lexing and
quote parity (simdjson); copy-and-patch stencils (Xu & Kjolstad); scans as the universal
primitive (Hillis & Steele, Blelloch). Codex's verdict was that the novelty is "the
combination and restriction"; I mostly agree. The pieces I have not seen elsewhere:

1. **The stack depth of a postfix line is the register number.** One prefix sum over
   token deltas allocates every zmm register in the program. Lines must end at depth 0,
   so the scan never resets. There is no parser, no tree and no allocator to run.
2. **Scatter-race symbol resolution.** All name tokens scatter their own index into a
   table at once; AVX-512's ordered scatter makes the last writer win; each token gathers
   the slot back and joins the winner if fingerprints match; losers re-race under another
   key. No sort, no sequential insertion. A free by-product: every name's canonical token
   is its *last* occurrence.
3. **Liveness, escape analysis and hoisting with no analysis pass**, all from that
   by-product and the absence of loops:
   - a vector is dead after the line of its last occurrence, so the compiler emits a
     release there (cut page faults by more than half);
   - a tee'd vector whose last occurrence is on its own line never leaves the loop, so it
     keeps only the current 64-byte chunk in its slot (24 of the compiler's vectors);
   - every token may carry a prologue stencil; a line is laid out as
     `[prologues][bodies][releases]` by arithmetic on three prefix sums. That is
     loop-invariant code motion in which nothing moves.
4. **Keywords by self-join**: a prelude line listing the operators is prepended to the
   source, so operators are ordinary names and "is a keyword" is "your canonical token is
   in the prelude". No keyword table and no string comparison in the compiler.
5. **Code generation as gather + scatter**, with nothing to fix up: every output byte
   gathers its stencil byte through a running-max fill; operands are scattered in; loops
   close through a register, so no branch offset depends on layout; the ELF file is a
   concatenation, so no header is back-patched.
6. **A probabilistic algorithm made part of the language definition** so that it can be
   differentially tested exactly: the race is specified in `boot/spec.py` and the scalar
   references *simulate* it. That was forced on me by review (below), and I think it is
   the right answer to "your SIMD trick is not complete".

## What is measured

Everything below is reproducible with `./build.sh`, `./test.sh`, `tools/prof.c`,
`tools/bench.c`. Details and method: SCALAR_LEDGER.md, HARDWARE.md.

**The machine** (HARDWARE.md). Zen 4 under KVM. CPUID is honest: all advertised AVX-512
subsets execute, FP16/VP2INTERSECT/AMX/APX fault. zmm = exactly 2x ymm. `vpcompressd` to
memory is 16x slower than to a register. Gather costs a scalar load loop; scatter costs
more than scalar stores. No frequency penalty.

**Correctness.**
- Fixed point: stage1 == stage2 == stage3, and == the C compiler's output.
- `./test.sh` on each of the three stages: 83 hand cases (each also executed against the
  interpreter); compiler-bytes fuzz against two scalar oracles with valid, re-aligned
  (all 16 offsets) and mutated programs — every error number except 4 and 5 has a
  hand case, and the fuzzers reach all of those too across seeds; semantic fuzz of compiled binaries against the interpreter with input
  lengths 0, 1, 15, 16, 17, …, 4097, 20001; a 346 KB and a 40,000-name program through
  all three compilers.
- `tests/mutation.sh`: five injected bugs (wrong compare, freeing static data, lost
  `prev` carry, broken scan, premature release) are each caught.
- Deterministic: identical bytes across runs, with ASLR off, with an empty environment,
  with unlimited stack, and from a pipe.

**SIMD density** (exact, ptrace single-step, self-compile):

| | first working version | now |
|---|---|---|
| instructions executed | 5.34 M (30 KB source) | 4.50 M (44.7 KB source) |
| vector | 31% | 61.5% |
| scalar bookkeeping | 61% | 29.9% |
| hand-written scalar code (stub) | 67 instr | 82 instr |

**Speed**, against `boot/strandc.c` (gcc -O2):

| input | scalar C | Strand/SIMD | |
|---|---|---|---|
| 44.7 KB (itself) | 1.83 ms | 2.61 ms | 1.43x slower |
| 346 KB | 11.0 ms | 21.0 ms | 1.9x slower |
| 2.79 MB | 127 ms | 200 ms | 1.57x slower |

**Where the time goes.** Not where the instructions go. A minor page fault costs about
1.45 us in this VM (writing 7.4 MB of fresh memory: 2.9 ms). The first compiler took 1,893
of them — 80% of its run time. Now 816, still roughly 45% of the run. The scalar compiler
takes 563.

## What failed

1. **The headline hope: data-parallel is not faster here.** A whole-array language
   materialises intermediates; on this machine fresh memory is the expensive thing, and
   `i32`-only lanes make every byte array 4x bigger than it needs to be. I chose one
   element type for simplicity before I had measured page-fault cost. HARDWARE.md had
   already told me byte lanes were 4x denser and I under-weighted it.
2. **Gather/scatter as the workhorse.** They made the design simple and they are not
   parallel on this CPU. I knew this from the probe before writing a line of the compiler
   and leaned on them anyway; the design would be different on hardware with real gathers.
3. **My first name hash was a linear 32-bit sum, and the race was unspecified.** I
   claimed byte-identical behaviour "on every input". Codex produced a two-line program
   that the SIMD compiler rejected and the reference accepted, and another with 40,000
   names. My fuzzers used short counter names and could never have found either.
4. **`build.sh` could not fail.** `cmp a b && echo ok` under `set -e`. The fixed-point
   check that this whole project is about was decorative until review 2.
5. **The first hardware probe was wrong.** gcc constant-folded the test instruction, so
   AVX-512FP16 "executed" on a CPU that does not have it. The same first version also
   overflowed L1 and reported `add` at 2.7x its real cost.
6. **My first fuzzer OOM-killed the VM** (and this session): a random program did `~N`
   with N ~ 10^9 and the Python interpreter built the list.
7. **Not SIMD at all**: instruction *encoding*. The compiler copies stencil bytes that GNU
   as produced. Calling that a "SIMD code generator" is fair for selection, layout and
   patching, and not for encoding.
8. **Still untested**: E_RACE and E_COLLISION (I cannot construct inputs), and the
   resource limits. The interpreter shares its lexer with the Python reference compiler.

## What the review process changed

REVIEWS.md has both rounds point by point. Round 1 (ideas, before the design was fixed):
I kept my own design and adopted two of Codex's warnings (specify diagnostic order;
test malformed input). Round 2 (adversarial, on the artifacts): three real defects
(items 3 and 4 above, and unchecked resource limits) and several fair hits on overclaims
and test blind spots; all fixed or recorded. Its suggested performance order (fuse
passes; byte lanes; scalar lines) matched mine for the first and third; the second is
still open.

## What I would do next

1. **Byte lanes.** A `u8` vector type with 64 lanes per zmm for the front end:
   `vpermi2b` classification, `vpcompressb` to a register. HARDWARE.md says 40x per byte
   for classification; the allocator model says the 7 byte-sized arrays would shrink from
   ~43 pages each to ~11. This is the change most likely to put the SIMD compiler ahead.
2. **A segmented scan** (`sscan`: running sum that restarts on a flag). The compiler
   currently gets per-token sums by compressing a running sum at both ends of each token
   (seven tee-compresses in one loop); a segmented scan halves that.
3. **Blocked execution.** Run the byte-level front end over cache-sized blocks of the
   source with carries between blocks, so the working set stays in L2 regardless of input
   size. Whole-array semantics do not require whole-array evaluation.
4. **Write the encoder in Strand.** Each stencil is a fixed instruction sequence; EVEX
   encoding is bit arithmetic on (opcode, registers, depth). Generating `sten` from a
   table of mnemonics *in Strand* would remove island 11 and make the fixed point cover
   the instruction encodings too.
5. **A complete fallback for names**: a 1-bit split radix sort is expressible in Strand
   today (60 unrolled passes of scan + scatter) and would make E_RACE impossible.
6. **An interpreter that shares nothing** with the reference compiler, and error messages
   with line numbers (the compiler currently exits with a number; `tools/lint.py` and
   `tools/where.py` do the locating in Python).

## Addendum: user time vs kernel time (a correction)

Above I say the gap to scalar C "is page faults, not instructions". Splitting user from
kernel time (`tools/bench.c` now reports both) shows that is only half right. On the 2.9 MB
input:

| | user | kernel | faults |
|---|---|---|---|
| scalar C, whole compile | 75 ms | 57 ms | 30,682 |
| Strand, whole compile | 122 ms | 80 ms | 48,857 |

Page faults are the largest single cost for *both* compilers (about 40% of each), and
Strand takes 1.6x as many. But Strand is also 1.6x slower in pure user time, so removing
every fault would not close the gap. By phase, stopping the Strand compiler early:

| Strand, cumulative | user | kernel |
|---|---|---|
| byte classes + token boundaries + line numbers | 12 ms | 27 ms |
| + per-byte token joins: digit values, two hashes, pool index | 48 ms | 52 ms |
| + name race, keywords, slots | 66 ms | 66 ms |
| + classes, depth, layout, code bytes, ELF | 122 ms | 80 ms |

For comparison the C compiler's lexing loop (tokens, interning, numbers) is 21 ms user.
So the scan-shaped front end is where SIMD is ahead (4 ns/byte even with i32 lanes); the
join-shaped work (bytes looking up their token through gathers; many narrow passes over
token arrays) is where it falls behind. No input shape I tried reverses the total:
comment-heavy, data-heavy, string-heavy and dense-code inputs are all 1.7-2.7x slower.

## Addendum 2: the sweet spot

Asked where this model *does* win, I built a benchmark suite and fixed what it exposed
(SWEETSPOT.md). The headline: single-pass map / filter / reduce / translate programs with
local context run at 6-8 GB/s on one core and 12-20 GB/s on the machine — 3-15x scalar C,
at least even with `gcc -O3 -march=native` per core, and 3-4x ahead of hand-written
mmap + OpenMP C wherever the loop carries context or filters. Pure maps and reductions tie
well-written parallel C at the kernel's I/O floor. The compiler itself, with eight vCPUs,
is now 15% faster than the scalar C compiler on a 5 MB input, and still slower on one core.
The cost is a hand-written runtime of ~790 instructions (SCALAR_LEDGER.md, items 15-20).
