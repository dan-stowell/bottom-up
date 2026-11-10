# SWEETSPOT.md — where Strand wins, where it ties, and why

*Rewritten after the audit (AUDIT.md, 2026-11-10). The first version of this file claimed a
3-4x lead over parallel C wherever a loop "carried a dependency". The audit showed that for
`words` that lead came from how my C baseline was spelled, not from anything C cannot do.
Every number below is from the rebuilt benchmark (`python3 bench/run.py 256`), whose C side
is now as strong as I can make it without intrinsics. Git history has the old file.*

## The short version

Against well-written C (`gcc -O3 -march=native`, mmap, OpenMP, loops shaped so that GCC
vectorises them) on this machine:

| class of program | Strand vs the best C | why |
|---|---|---|
| maps, reductions, neighbour tests (`prev`), prefix sums | **tie** (0.8-1.3x) | GCC vectorises these too, once the loop is spelled the right way; both sides then sit near the cost of getting at the file |
| table translation (256-entry lookup) | **1.7-2.3x** | GCC does not turn `out[i] = tab[in[i]]` into byte permutes |
| filters (output position depends on the data) | **1.6-3.9x** | GCC does not emit compress instructions for `if (keep) out[k++] = x` |
| **small automata** (comment stripping, CSV quoting, lexing) | **3.2-4.1x on one core, 2-5x on all** | a state machine is a loop-carried dependency no C compiler vectorises or threads; Strand runs it as a scan of transition functions |

So the sweet spot is narrower than first claimed and it has a name: **byte-stream
transducers with a small amount of state** — tokenising, stripping, classifying, filtering,
translating. That is also, not by accident, what the front end of a compiler does.

## What the advantages actually are

1. **The only loop-carried dependencies the language has are associative, by construction.**
   `prev`, `scan`, `scanmax` and the automaton `/t` are each a scan over a small monoid
   (shift, +, max, function composition). An associative carry can be computed inside a
   register without a loop and can be combined across thread ranges, so *every* line
   vectorises and *every* line can be split. A C compiler has to discover that property in
   a loop it was handed, and for anything past a simple reduction it does not.

2. **No cliffs from spelling.** `explore/cliff.c` counts words five obvious ways. One
   spelling vectorises (19.7 GB/s in memory); the other four run at 1.4-1.9 GB/s. The
   difference between the fast one and its nearest neighbour is whether `i == 0 ||` is
   inside the loop. That 10x is invisible in the source, and it is what made my first
   baseline slow. In Strand the slow versions cannot be written. (Strand has its own much
   smaller cliff, listed under "What does not work" below.)

3. **Shapes compilers do not vectorise at all**: data-dependent output position (filters),
   byte-table translation, and automata. These are where the measured gaps are.

4. **Threading comes with the line.** Splitting a filter needs per-thread regions written in
   order; splitting a prefix sum or an automaton needs two passes and a combine step. In
   `bench/base_omp.c` those are 12-25 lines each, by hand, per program. In Strand it is the
   same one line, and the runtime verifies the split against the serial result.

5. **Compilation is a table lookup.** Compiling one of these programs takes **0.22-0.24 ms**
   with the self-hosted compiler; `gcc -O3 -fopenmp` takes 66-90 ms on the C equivalent and
   `gcc -O0` 31 ms. Because every decision an optimiser would make is already fixed by the
   syntax, copy-and-patch output *is* the vectorised, threaded code. For one-off questions
   about a file, gcc's compile time alone is what Strand needs to process 0.2-0.7 GB.

What is **not** an advantage: raw speed on anything GCC already vectorises, and the
compiler itself (REPORT.md: 1.3-1.5x slower than scalar C on one core).

## The map (`python3 bench/run.py 256`)

Whole processes; 256 MiB from the page cache; output to /dev/null; 7 interleaved rounds;
GB/s of input as **median (best)**. "plain C" is `bench/base.c` (one thread, `read`),
"OpenMP C" is `bench/base_omp.c` (mmap, OpenMP). The last three columns divide Strand's
median by the best C median at the same CPU count. Every timed configuration's output is
hashed under its own CPU mask and must equal plain C's.

| program | shape | plain C | OpenMP C, 1 | OpenMP C, 4 | OpenMP C, 8 | Strand, 1 | Strand, 4 | Strand, 8 | Strand / best C, 1 | 4 | 8 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| lines | reduce | 4.92 (5.06) | 5.93 (6.68) | 8.24 (10.41) | 10.92 (13.43) | 6.36 (6.59) | 10.43 (11.13) | 10.91 (12.65) | 1.07x | 1.27x | 1.00x |
| lines32 | reduce, dword lanes | 4.98 (5.16) | 6.15 (6.51) | 9.77 (11.79) | 9.82 (11.62) | 4.97 (5.03) | 8.39 (9.91) | 9.52 (10.75) | 0.81x | 0.86x | 0.97x |
| words | prev+reduce | 5.26 (5.41) | 5.72 (6.29) | 8.53 (10.37) | 9.60 (10.02) | 6.50 (7.07) | 10.22 (11.20) | 10.78 (12.13) | 1.14x | 1.20x | 1.12x |
| sum | reduce | 5.34 (5.52) | 6.69 (7.18) | 9.18 (12.75) | 10.44 (11.27) | 6.70 (7.76) | 10.43 (11.54) | 11.35 (12.95) | 1.00x | 1.14x | 1.09x |
| pairs | prev+reduce | 5.09 (5.31) | 6.40 (6.56) | 8.33 (9.65) | 9.98 (11.16) | 6.74 (6.91) | 9.24 (10.19) | 10.46 (14.71) | 1.05x | 1.11x | 1.05x |
| nonascii | reduce | 5.09 (5.43) | 6.47 (7.23) | 8.40 (10.23) | 9.85 (12.65) | 6.89 (7.42) | 11.05 (11.55) | 12.64 (15.02) | 1.06x | 1.31x | 1.28x |
| upper | map | 5.81 (6.16) | 5.72 (5.86) | 10.31 (10.75) | 8.48 (10.92) | 5.28 (5.50) | 8.50 (10.50) | 8.61 (10.47) | 0.91x | 0.82x | 1.02x |
| rot13 | map | 4.27 (4.38) | 4.90 (5.11) | 7.03 (8.53) | 7.37 (9.01) | 4.83 (4.93) | 7.89 (9.19) | 8.28 (9.16) | 0.99x | 1.12x | 1.12x |
| swapcase | table lookup | 2.27 (2.31) | 2.36 (2.38) | 4.89 (6.39) | 5.14 (6.01) | 5.50 (6.02) | 9.89 (12.53) | 8.81 (13.57) | **2.33x** | **2.02x** | **1.72x** |
| maxline | scan+reduce | 0.94 (0.94) | 1.41 (1.41) | 3.54 (4.68) | 4.48 (8.00) | 2.70 (2.74) | 4.23 (5.29) | 3.92 (7.71) | 1.92x | 1.20x | 0.88x |
| letters | filter | 1.34 (1.37) | 1.48 (1.49) | 4.61 (4.93) | 3.86 (4.80) | 5.76 (5.93) | 7.92 (13.49) | 8.77 (13.70) | **3.88x** | **1.72x** | **2.27x** |
| sum4 | reduce | 7.46 (7.73) | 9.38 (9.55) | 10.91 (17.87) | 12.33 (13.01) | 9.48 (9.62) | 12.53 (16.04) | 12.88 (15.71) | 1.01x | 1.15x | 1.04x |
| sumpos4 | map+reduce | 7.51 (7.63) | 9.32 (9.44) | 11.42 (15.49) | 11.39 (11.61) | 8.92 (9.09) | 12.07 (14.87) | 11.70 (14.38) | 0.96x | 1.06x | 1.03x |
| maxgap4 | prev+reduce | 3.85 (3.86) | 9.74 (9.78) | 11.02 (11.35) | 11.74 (14.04) | 9.27 (9.46) | 14.51 (15.16) | 12.77 (20.85) | 0.95x | 1.32x | 1.09x |
| scale4 | map | 6.30 (6.41) | 7.37 (7.58) | 10.63 (11.53) | 10.49 (16.24) | 6.94 (7.15) | 10.61 (15.02) | 10.20 (12.47) | 0.94x | 1.00x | 0.97x |
| filter4 | filter | 3.61 (3.68) | 4.46 (4.49) | 6.59 (8.58) | 7.45 (8.59) | 7.38 (7.59) | 11.93 (13.84) | 11.68 (15.70) | **1.65x** | **1.81x** | **1.57x** |
| prefix4 | scan | 4.62 (4.76) | 5.01 (5.06) | 7.59 (11.76) | 7.89 (12.45) | 6.47 (6.68) | 8.28 (11.93) | 7.07 (10.60) | 1.29x | 1.09x | 0.90x |
| strip | automaton+filter | 0.88 (0.90) | 0.96 (1.05) | 0.92 (1.00) | 1.47 (1.52) | 3.09 (3.15) | 4.96 (7.17) | 5.87 (7.85) | **3.22x** | **5.40x** | **3.98x** |
| csv | automaton+reduce | 0.84 (0.84) | 0.78 (0.79) | 2.68 (2.89) | 3.20 (3.96) | 3.34 (3.39) | 5.74 (7.41) | 5.61 (10.62) | **3.98x** | **2.14x** | **1.75x** |
| idents | automaton+prev+reduce | 0.76 (0.79) | 0.78 (0.79) | 1.09 (1.11) | 1.69 (1.90) | 3.21 (3.27) | 3.24 (3.78) | 5.08 (9.66) | **4.11x** | **2.97x** | **3.01x** |
| strings | automaton, prev of state | 0.79 (0.90) | 0.72 (0.80) | 0.79 (0.88) | 0.91 (1.13) | 2.78 (2.91) | 3.77 (5.92) | 4.68 (6.79) | **3.53x** | **4.78x** | **5.14x** |

How to read it, and how not to:

- **Medians and bests differ a lot above one CPU.** This is an 8-vCPU VM; multi-thread
  timings vary by 20-50% between rounds. Ratios at 4 and 8 CPUs are indicative, not precise.
  The one-CPU columns are stable to a few percent.
- **Everything converges on 10-13 GB/s.** Mapping, faulting and unmapping 256 MiB costs
  roughly 20 ms here however it is done (`explore/iotest.c`), so no whole-process number
  can be much above that. A lead in compute shrinks as both sides approach it, which is why
  the ratios at 8 CPUs are smaller than at one for the programs where C scales. This is an
  observation about these runs on this VM, not a proved lower bound.
- **The four automaton programs and the C beside them**:

```
# strip: delete '#' comments, except inside "..." strings (with \ escapes)
~256 :i 35 = i 34 = 2 * + i 10 = 3 * + i 92 = 4 * + >cls     # byte -> class: other # " newline \
0 1 2 2  1 1 2 2  2 1 0 2  0 0 2 2  0 1 3 2 =dfa             # next state, by class and state
bytes dup @cls /dfa 1 != outif                               # keep bytes whose state is not "comment"

# idents: words of [A-Za-z0-9_] that begin in code
bytes :c @cls /dfa 0 = c @isw dup prev 1 ^ & & $N
# strings: entries into the string state
bytes @cls /dfa :s 2 = s prev 0 = & $N
# csv: separators outside quotes
bytes @cls :c /q 0 = c 2 = & $N
```

  The C versions are a `switch` in a loop (the fastest of five formulations I tried for
  `idents`; a transition table is slower, 0.49 against 0.95 GB/s in memory). The OpenMP
  versions run each range from all four states to learn its transition function, then run
  it again from the right one. That is the same idea Strand uses, written out by hand, and
  for `strip` and `strings` it does not pay until eight threads because the first pass is
  four times the work. A C programmer with intrinsics and the technique below would match
  Strand; `explore/fsm.c` is that program (6.1 GB/s in memory against 1.2 for the switch).

## How the automaton runs (`x /t`)

`x /t` replaces each byte by the state of a 4-state automaton after reading it:
`state' = t[(x & 15) * 4 + state] & 3`, starting from state 0.

A transition function on 4 states is 4 bytes, so it fits one dword lane, and composing two
of them is one `vpshufb`. The stencil (about 50 instructions, no loop, no branch on data):

1. The 64 bytes of a chunk are 16 groups of 4. Register *t* (of four) gets the functions of
   bytes 4g+t, one dword per group g: a `vpermd` from a 16-entry table, indexed by class.
2. Three `vpshufb` compose the four registers in sequence. Every group advances at once, so
   this is a 3-step chain for all 64 bytes, and the fourth register now holds the 16 group
   functions in order.
3. A Kogge-Stone scan (four rounds of shift + `vpshufb`) turns those into prefix functions.
4. The carry — the function from the start of the line to this chunk — is composed in, the
   state for start state 0 is read out of each function, and the new carry is the last one.

The first prototype kept one function per byte in four registers and scanned across all of
them: 99 vector operations per chunk, 3.1 GB/s. Transposing the layout so that the first
two levels of the scan are a chain *across* registers cut it to 36 and doubled the speed.
On Zen 4 a 512-bit operation costs two 256-bit ones (HARDWARE.md), so the count is what
matters.

**Across threads** the carry is a function, so ranges compose. The pool's dry pass gives
each range's function, `IN[t] = OUT[t-1] o IN[t-1]`, and the real pass starts each thread
in the right state; the existing verification (every thread's starting carries equal its
predecessor's ending carries) covers it. Two refinements, both in the stub:

- **The dry pass stops early.** It only needs the carry. Each carry-using token's prologue
  clears a word and the automaton's writes its id there, so at run time the word names the
  automaton iff it is the last carry on the line; in a dry pass that token jumps to the
  loop end after updating its carry. `strip` went from 3.5 to 5.1 GB/s on four CPUs.
- **Carries that depend on other carries.** `s prev` after an automaton (where does the
  state change?) depends on states the dry pass cannot know, and used to fall back to one
  thread. The dry pass now also reports carries one chunk before the end of each range;
  from those the pool derives every thread's carries one chunk *before* its range; the real
  pass begins with a warm-up chunk from them, which makes the `prev` carries exact. The
  same fix made `prev` of a running sum parallel (44 ms on one CPU, 19 on eight).

The compiler's own source did not change for any of this: one sigil, two classes, one
stencil, table entries. The fixed point moved and still holds.

## What does not work

- **The compiler itself** is not in the sweet spot (REPORT.md). The earlier "15% faster than
  scalar C on 8 vCPUs for a 5 MB source" did not reproduce in the audit's two batches
  (Strand 257-301 ms, C 234-260 ms) and is withdrawn.
- **Maps, reductions and neighbour tests are ties**, not wins. The old `words`, `pairs` and
  `maxgap4` leads were baseline artefacts.
- **Strand's own cliff.** When one scan's input depends on another scan's values (a running
  sum of a running sum, an automaton fed by an automaton), the dry pass is a guess. If the
  guess is wrong, verification fails and that block finishes on one thread: still correct
  and still vectorised, but at the one-CPU rate, and nothing in the source says so. (It
  often does pass: an automaton fed by another ran 2.5x faster on eight CPUs than on one in
  a spot check, because automata tend to forget their starting state.)
- **Four states, sixteen classes, byte lines only.** UTF-8 validation (9 states), C
  comments with strings and character literals (9) and anything with a counter do not fit.
  Eight states would need 8-byte functions in qword lanes; not built.
- **Scaling of automaton lines is modest**: 3 GB/s on one CPU, 5-8 on eight. The dry pass
  costs about half a real pass, and the stack-machine stencils are less tightly fused than
  hand-written code (`explore/fsm.c` reaches 6.1 GB/s on one core in memory; the Strand
  program's compute rate is about 4.5-5).
- **Outside the model entirely**: histograms and anything else needing scatter-add;
  data-dependent control flow (nested structure, early exit); pointer chasing; joins
  (expressible through gather/scatter, which cost scalar-loop prices on this CPU).
- **Several reductions in one line** (a real `wc`) are still two passes.

## Mistakes along the way (kept, because they are the method)

- **The words claim.** I measured a slow C loop and explained it with a general statement
  ("the compiler cannot vectorise a carried dependency") without checking whether another
  spelling vectorised. The audit did. `explore/cliff.c` is the experiment I should have
  run first; `bench/base_omp.c` and `bench/base.c` now use the fast spelling.
- **A fast benchmark of a failed program was possible.** `tools/bench.c` ignored its
  child's exit status. It now fails instead, and `run.py` hashes every configuration.
- **The new check caught my own baseline within minutes**: I replaced `#define`s with an
  `enum` in `base_omp.c`; the preprocessor read the names as 0 and every kernel compiled as
  `lines`. The per-configuration output hash refused to time it.
- **My first `idents` baseline was 2.6x slower than it needed to be** (range comparisons
  instead of a class table). Five formulations are in the commit history; the fastest is
  the baseline.
- **Earlier ones** (git history has the detail): filters first lost by 2x until `outif`
  streamed them; the first parallel `prev` stored its warm-up chunk; the main thread's
  stack was misaligned inside the pool; a fully masked store to unmapped memory took a
  microcode assist per chunk.

## Where this points

The model's edge is carried state that is associative but that a compiler cannot see as
such. Automata are one instance. Others with the same structure, not built:

- **8- and 16-state automata** (UTF-8 validation, fuller lexers), with wider functions.
- **Segmented scans** (a sum that resets at each newline) are a scan over "add or reset"
  functions; per-line and per-field aggregates would follow.
- **Number parsing** is a scan over affine maps (x -> 10x + d).
- **An automaton on dword lanes**, so that the compiler's own lexer (comments, strings,
  token boundaries) becomes one line instead of the prefix-count construction in DESIGN.md.
