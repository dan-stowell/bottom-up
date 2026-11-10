# A compiler with no loops in it

*Checkpoint 1: the language is feasible. 2026-11-10.*

The brief was to design a language whose compiler runs as SIMD code in every phase, then
write that compiler in the language. I started from the end: **what would the compiler's
source have to look like if it had no loops and no branches at all?**

It would be a straight list of statements, each of which transforms whole arrays. So that
is the entire language. Strand has no `if`, no `while`, no functions. A program is a list
of lines; every line compiles to one fused AVX-512 loop over `i32` lanes. This is a
complete program that uppercases its input:

```
input >s
s 97 >= s 122 <= & >islow
s islow 32 * - out
```

## Bending the language until the compiler is flat

Lines are postfix. That one decision deletes three compiler phases:

- **No parser.** Nothing nests, so there is no tree. A token is a run of non-blanks.
- **No register allocator.** In postfix, a value's stack depth *is* its register:
  `depth = scan(delta[kind])`, and a token at depth `d` works on `zmm(d-1)`. One prefix sum
  allocates every register in the program. Lines must end at depth 0, so the scan never
  even resets between lines.
- **No type inference.** `abc` is a vector, `Abc` a scalar, and a sigil says what a token
  does (`>x` store, `@x` gather, `?x` compress-store, `!x` scatter, `$S` sum).

The parts that are sequential in every compiler I know needed something else:

**Symbol table → scatter race.** Every name token hashes itself (a segmented polynomial
hash is the difference of two prefix sums), then all of them scatter their own index into
`table[hash]` at once. AVX-512 scatters are ordered, so the last writer wins,
deterministically. Each token gathers the slot back: if the winner has my hash, it is my
canonical occurrence. Names that lost to a *different* name race again in a second and
third table. A final gather compares every name byte against its canonical occurrence, so
a hash collision is a compile error, never a miscompile. No sort, no insertion order.

**Keywords → self-join.** The compiler prepends a line containing every operator name to
the source. Operators are then ordinary names, and "is a keyword" means "your canonical
occurrence is in the prelude".

**Code generation → gather and scatter.** Each (operation, depth) has a pre-assembled
stencil. Sizes are prefix-summed into offsets; every output byte gathers its stencil byte;
operand displacements are scattered in. Loop back-edges go through a register, so no
branch offset ever needs fixing up. The ELF file is emitted by concatenation, so no
header is back-patched either.

## It works

- `boot/strand.py` is a naive scalar compiler and, separately, an interpreter (Python).
  `boot/strandc.c` is the same compiler again in plain C.
- `src/compiler.strand` is the compiler in Strand: ~230 lines, straight-line.
- Compiled programs agree with the scalar interpreter on thousands of random programs
  with inputs sized 0, 1, 15, 16, 17, 31, 32, 33, ... (chunk boundaries).
- The Strand compiler agrees with both scalar compilers **byte for byte** on random valid
  programs and on mutated, malformed ones (same error number).

## What the measurements said (not all of it flattering)

HARDWARE.md came first and it mattered. On this Zen 4: zmm is exactly 2× ymm (so AVX-512
is a better instruction set, not a wider one); `vpcompressd` to memory is 16× slower than
to a register; and **gather and scatter cost exactly what a scalar loop costs**. My design
leans on gather/scatter, so those are branch-free but not fast, and SCALAR_LEDGER.md
counts them as serial islands.

Then I profiled the working compiler and found two things I had not designed for:

1. **80% of compile time was page faults.** Nothing was ever freed. Touching fresh memory
   costs ~1.5 µs/page in this VM. The fix fell out of the language: with no loops, a
   vector is dead after the line containing its *last occurrence* — and the scatter race
   already computes exactly that token. Liveness analysis is free. The compiler now emits
   a release there.
2. **61% of executed instructions were scalar bookkeeping** — mask setup, pointer loads,
   length checks, per iteration. Not SIMD-dense. Now the lane mask is one `vpcmpud`, and
   each token can carry a *prologue* stencil that is laid out before its line's loop:
   three code streams interleaved purely by arithmetic on prefix sums. That is
   loop-invariant code motion with no motion pass. Bookkeeping fell to 37%, vector
   instructions rose to 55%.

And the number that keeps me honest: the SIMD compiler compiles its own 37 KB source in
3.5 ms. The obvious scalar C compiler does it in 1.4 ms. **Data-parallel is not yet
faster.** The compiler makes ~250 separate passes over memory where C makes about five.
The next round is about fusing passes.

Two bugs worth recording: my first hardware probe "proved" the VM executes AVX-512FP16,
because gcc had constant-folded the instruction away; and my first fuzzer generated
`~N` with N ≈ 10⁹, and the Python interpreter OOM-killed the whole VM session.

## Update, same day

Fusing passes is done: `:x` ("tee") stores a named vector without ending the line, so the
byte-level front end is 3 loops instead of ~40 and the compiler is 149 lines instead of
232. Vector instructions are now 61% of what executes. It is still slower than scalar C
(1.7x, was 2.5x); REPORT.md has the final numbers and the reason, which is page faults,
not instructions.
