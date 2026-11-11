# RUNTIME.md — what the stub is, and what a program's output can depend on

*2026-11-11.*

Every Strand binary starts with the same blob, `stencils/stub.S`: about 1,500 instructions
of hand-written assembly. It is what the language cannot say: system calls, threads, and
the pool that runs one line on several cores. It has grown with every round of speed work,
and assembly that grows is assembly nobody can vouch for. This file says what in it has to
be right, what does not, and how the second claim is tested.

## The one rule

A line's loop body is a function: (elements of a range, carries at its start) to (results,
carries at its end). The carries are the line's running sums and maxima, its `prev`
values, the states of its automata, the counts of its sinks. A serial run is the ranges in
order, each starting from the carries the one before ended with.

The pool runs the ranges of a block at the same time, so every range but the first must
start from carries nobody has computed yet. However it gets them, **the block is kept only
if every range turns out to have started from exactly the carries the range before it ended
with** (`fs7`); otherwise the first range's true carries are restored and the block is
redone. If that comparison is right, a kept block is a serial run cut into pieces.
(A line with no carries at all has nothing to compare and is simply cut up. Until the review
below, a line whose only carries were `prev` was cut up the same way, on the argument that
a one-chunk warm-up always makes `prev` right; true, but a second rule. Such lines, and
lines that write to stdout, now go through the comparison like the rest.)

Everything else the pool does is a way of making the comparison succeed more often: the
warm-up before a range, the state an automaton is guessed to be in, the dry pass, deriving
one range's carries from another's results, deciding which ranges to rerun after a miss.
None of it can make a wrong block pass. It can only cost time.

## Testing that it is so: chaos

A claim like that about assembly is worth what its test is worth. Run any Strand binary
with two arguments and the stub spoils its own guesses:

- about half the ranges of every pass start from carries xor-ed with noise, whether the
  carries were guessed or derived;
- the decision which ranges to rerun is flipped at random;
- in every range, the first included, the parts of a carry that the comparison does *not*
  look at (all but the last element of a `prev` carry) are overwritten: the comparison
  claims nothing else is read.

The output must be byte for byte what one CPU produces. `tests/pool.py` runs every program
this way as well as normally (10,912 runs), `tests/fuzz_sem.py` every random program,
`tests/pool_big.py` its large inputs, and `test.sh` the Go lexer and the declaration
index. `tests/pool_big.py` also checks that under chaos the retry and the serial fallback
are reached on an input where they never are otherwise, so the noise is real. (The lexer
under chaos on eight cores takes about four times as long, and writes the same tokens.)

Two things are deliberately not spoiled, because they are not guesses. The first range's
carries are the caller's. And a sink's count is where the sink writes: on a one-pass line it
is the address of a scratch area the pool chose, on any other line it comes from the dry
pass and a wrong one would write outside the vector before any comparison could object.

What chaos is not: a proof. The noise is one random dword per range, not every possible
wrong carry; and it tests the claim only for the stencils and programs the tests run.

## What is left to trust

`tools/stubmap.py`:

| part of the stub | instructions | can it change a program's output? |
|---|---|---|
| start: CPUs, heap, stacks, input | 179 | yes |
| output, exit, failure report | 93 | yes |
| threads: start, wake, wait | 98 | yes |
| split a line into blocks and ranges | 166 | yes |
| run one range from given carries | 336 | yes |
| verify: every range started where the one before ended | 78 | yes |
| one-pass lines: scratch areas, copy into place | 167 | yes |
| a block that failed: carries restored, run serially | 42 | yes |
| guess: warm-up, states and prev carries of a range | 151 | no: spoiled by `chaos`, output unchanged |
| choose dry pass or one pass; derive carries; which ranges to rerun; retry | 228 | no: spoiled by `chaos`, output unchanged |
| chaos (tests) | 27 | no: spoiled by `chaos`, output unchanged |
| **total** | **1568** | 1159 yes, 409 no |

So about a quarter of the stub, and nearly all of what the last three rounds of speed work
added, is now outside what the output depends on. What remains is of two kinds. Half is
plumbing that fails loudly when wrong (start-up, output, threads). The other half is the
pool's kernel: cutting a line into ranges, running a range from given carries, the
comparison, the copy of a one-pass line's results into place, the serial fallback. That is
some 740 instructions, it has one rule to keep, and it is the part to review; nothing
checks it but tests against the one-CPU run.

## Where the language comes in

What the comparison needs to know about a carry is a fact about the language, not about
the stub: a sum is compared whole, a `prev` by its last element, an automaton by its
state. That is now written down once, in `boot/spec.py` (`CARRYKIND`, `CARRYBITS`), next to
the table that says which classes have a carry at all; the stencils' declarations to the
stub are generated from it instead of being typed into each prologue.

The larger observation is that the policy half is the language's own arithmetic one level
up. Deriving the carries at the start of each range from every range's effect is a prefix
sum over ranges for sums, a prefix composition for automata, a shift for `prev`: `scan`,
`/t`, `prev`, over a vector with one element per thread. It is written in assembly because
the pool runs in the middle of a line, with every register in use. Now that it cannot
affect the output, it can be rewritten, simplified or thrown away without anyone having to
believe the result, and the natural rewrite is in Strand.

## Reviewed

`REVIEW_SOL.md` is a review of the kernel by another model (gpt-6.1-sol), asked whether a
block can be kept without the comparison covering every carry a range read, whether the
comparison is right for each kind of carry, whether the scratch and copy arithmetic can go
wrong, and whether chaos is a fair test. It found no wrong output and two things, both
acted on: lines with only `prev` carries bypassed the comparison (above), and a byte line's
`prev` was compared by its last four bytes when only the last is read, which cost retries
and left three bytes that chaos never spoiled. It also checked the scratch-area and copy
arithmetic by hand and found it sound for up to 16 threads.
