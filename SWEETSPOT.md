# SWEETSPOT.md — where Strand wins, where it ties, and why

*Third version. The first claimed a 3-4x lead over parallel C wherever a loop "carried a
dependency"; the first audit (AUDIT.md) showed that lead came from how my C was spelled. The
second version called its C "the strongest without intrinsics"; the second audit
(AUDIT_FOLLOWUP.md) found one more loop that was not, and asked the better question: how
does this compare with what an expert writes? This version answers it with a third
implementation of every program where ordinary C loses, hand-written with AVX-512
intrinsics (`bench/hand.c`). Every number is from `python3 bench/run.py 256` on this
machine. Git history has the old files.*

## The short version

Strand sits between what GCC makes of ordinary C and what an expert writes with intrinsics:

| class of program | Strand vs ordinary C | Strand vs hand-written intrinsics | why |
|---|---|---|---|
| maps, reductions, neighbour tests (`prev`), prefix sums | **tie** (0.7-1.3x; `maxline` 1.9x on one core only) | not written; a tie is the expectation | GCC vectorises these once the loop is spelled the right way |
| byte-table translation (a table that is not arithmetic) | **1.6-2.3x** | 0.8-0.9x | GCC does not turn `out[i] = tab[in[i]]` into byte permutes |
| filters (output position depends on the data) | **1.4-3.8x** | 0.8-1.2x | GCC does not emit compress for `if (keep) out[k++] = x` |
| small automata, same technique by hand (strip, idents, strings) | **3.0-5.6x** | 0.5-0.9x | no compiler turns a state machine into a scan of transition functions |
| small automata where a better algorithm is known (csv, json) | **2.2-7.3x** | 0.4-0.6x (1.2x in one cell) | quote parity by carry-less multiply beats any automaton; Strand has no such primitive |

"Ordinary C" is the faster of `bench/base.c` (one thread, `read`) and `bench/base_omp.c`
(mmap, OpenMP, loops shaped to vectorise), on at most the same number of CPUs. These are
the implementations I could write, not an upper bound on C; `swapcase` was in the table
class until the second audit rewrote it as arithmetic, where it ties.

So the sweet spot is this: **byte-stream programs with a little associative state, where
the fast code exists but a compiler will not produce it from ordinary source.** There,
one line of Strand, compiled in a quarter of a millisecond, gets 75-95% of hand-written
intrinsics on one core when both use the same technique, and 1.4-7x what GCC gets from a
loop. It does not beat an expert, and it loses by 2x when the expert knows a better
algorithm.

## What the advantages actually are

1. **The only loop-carried state the language has is associative, by construction.**
   `prev`, `scan`, `scanmax` and the automaton `/t` are each a scan over a small monoid
   (shift, +, max, function composition). So every line vectorises without analysis, and a
   line can be split across threads by combining per-range summaries. A C compiler has to
   discover that property in a loop it was handed. (Splitting has a limit of its own, under
   "Dependent carries" below: it is not true that every line runs on all cores.)

2. **Fewer cliffs from spelling.** `explore/cliff.c` counts words five obvious ways; one
   vectorises (19.7 GB/s in memory), four run at 1.4-1.9. `swapcase` as `tab[c]` runs at
   2.4 GB/s and as `c ^ (((u >= 65) & (u <= 90)) * 32)` at 4.9. I wrote the slow form both
   times, in a benchmark, while trying to write fast C; that is the evidence that the cliff
   is real, and also why this file's claims about C kept being wrong. In Strand a table
   lookup is a byte permute, so the two spellings of `swapcase` run at about the same speed
   (5.5 and 5.0 GB/s).

3. **Shapes compilers do not vectorise at all**: data-dependent output position, byte-table
   translation, automata. These are the measured gaps.

4. **Threading comes with the line.** In `bench/base_omp.c` and `bench/hand.c`, splitting a
   filter, a prefix sum or an automaton is 12-25 lines each, by hand, per program (and the
   one for JSON had a bug that only the output check on four CPUs caught). In Strand it is
   the same one line, and the runtime verifies the split against what one thread would do.

5. **Compilation is a table lookup.** 0.21-0.24 ms per program, against 64-90 ms for
   `gcc -O3 -fopenmp`. From source text to answer, for a question asked once
   (`bench/oneoff.py`: compile, then run, one CPU, median of 7, ms):

   | program | input | Strand | gcc -O3 `base.c` | gcc -O3 -fopenmp `base_omp.c` |
   |---|---|---|---|---|
   | words | 1 MiB | 5.6 | 57.5 | 71.7 |
   | words | 256 MiB | 36.7 | 94.3 | 108.1 |
   | strip | 1 MiB | 5.3 | 49.7 | 95.1 |
   | strip | 256 MiB | 87.5 | 338.8 | 350.6 |
   | json | 1 MiB | 4.7 | 50.7 | 89.4 |
   | json | 256 MiB | 86.9 | 542.4 | 569.5 |

   Both sides include the measuring script's two process launches, which alone take 4-6 ms:
   at 1 MiB the Strand column is almost entirely that. `words` is a tie in the main table
   and 10x apart here at 1 MiB: for small inputs the compiler is the cost, and this is the
   one advantage that does not depend on the program's shape.

What is **not** an advantage: speed on anything GCC already vectorises; speed against
hand-written intrinsics; and the compiler itself (REPORT.md: 1.3-1.5x slower than scalar C
on one core).

## The map (`python3 bench/run.py 256`)

Whole processes; 256 MiB from the page cache; output to /dev/null; 7 interleaved rounds;
GB/s of input as **median (best)**; CPUs 1, 4 and 8 of 8. The ratio columns divide
Strand's median by the fastest C median on at most that many CPUs. Every timed
configuration's output is hashed under its own CPU mask and must equal plain C's; for
`json`, plain C's must also equal a skeleton rebuilt from Python's `json` parser.
`build/suite/results.json` keeps every sample, the input hashes, the compiler version and
the source revision. (`json` was timed in a second invocation right after the other 22;
see "Mistakes".)

| program | shape | plain C | OpenMP C, 1 | 4 | 8 | hand C, 1 | 4 | 8 | Strand, 1 | 4 | 8 | Strand / ordinary C (1, 4, 8) | Strand / hand C (1, 4, 8) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| lines | reduce | 5.43 (5.52) | 6.93 (7.21) | 8.22 (10.92) | 9.11 (10.15) | - | - | - | 6.59 (6.92) | 10.82 (12.05) | 10.88 (13.78) | 0.95x / 1.32x / 1.19x | - |
| lines32 | reduce, dword lanes | 5.63 (5.64) | 7.03 (7.31) | 8.30 (11.95) | 9.64 (11.02) | - | - | - | 5.07 (5.23) | 7.44 (8.21) | 8.85 (8.95) | 0.72x / 0.90x / 0.92x | - |
| words | prev+reduce | 5.40 (5.46) | 6.30 (6.72) | 8.28 (11.19) | 9.30 (10.99) | - | - | - | 6.85 (7.14) | 9.79 (12.18) | 10.11 (11.00) | 1.09x / 1.18x / 1.09x | - |
| sum | reduce | 5.49 (5.58) | 7.01 (7.21) | 8.55 (9.23) | 9.83 (11.08) | - | - | - | 7.40 (7.87) | 10.27 (11.26) | 11.35 (12.45) | 1.06x / 1.20x / 1.16x | - |
| pairs | prev+reduce | 5.42 (5.63) | 6.49 (6.65) | 7.63 (8.59) | 9.93 (10.18) | - | - | - | 6.64 (7.27) | 8.50 (9.89) | 10.15 (13.78) | 1.02x / 1.11x / 1.02x | - |
| nonascii | reduce | 5.37 (5.57) | 7.10 (7.32) | 8.28 (9.47) | 9.88 (11.67) | - | - | - | 7.54 (7.73) | 8.52 (10.92) | 10.44 (12.07) | 1.06x / 1.03x / 1.06x | - |
| upper | map | 5.78 (6.18) | 5.61 (5.75) | 8.52 (10.32) | 8.79 (10.16) | - | - | - | 5.18 (5.57) | 7.93 (8.57) | 9.18 (9.40) | 0.90x / 0.93x / 1.04x | - |
| rot13 | map | 4.54 (4.68) | 4.95 (5.00) | 7.16 (7.82) | 8.06 (9.62) | - | - | - | 4.76 (5.13) | 7.44 (8.45) | 6.57 (8.69) | 0.96x / 1.04x / 0.81x | - |
| swapcase | map (was a table) | 4.45 (4.51) | 4.95 (5.38) | 6.94 (9.76) | 8.10 (9.10) | - | - | - | 4.98 (5.39) | 7.73 (7.92) | 9.12 (10.40) | 1.01x / 1.11x / 1.13x | - |
| xlat | table lookup | 1.43 (1.44) | 2.36 (2.37) | 4.31 (5.20) | 5.41 (7.73) | 5.95 (6.60) | 9.91 (11.87) | 9.07 (10.54) | 5.51 (5.89) | 7.53 (8.04) | 8.87 (9.41) | **2.33x / 1.75x / 1.64x** | 0.93x / 0.76x / 0.89x |
| maxline | scan+reduce | 0.94 (0.95) | 1.41 (1.41) | 3.96 (4.80) | 4.47 (6.08) | - | - | - | 2.72 (2.73) | 3.42 (5.17) | 4.46 (5.45) | 1.94x / 0.86x / 1.00x | - |
| letters | filter | 1.37 (1.40) | 1.47 (1.50) | 3.89 (4.86) | 4.12 (4.97) | 6.04 (6.67) | 12.20 (12.97) | 9.72 (14.23) | 5.58 (6.06) | 9.76 (13.01) | 10.57 (14.64) | **3.78x / 2.51x / 2.57x** | 0.92x / 0.80x / 0.87x |
| sum4 | reduce | 7.48 (7.59) | 8.58 (8.94) | 11.25 (11.96) | 11.53 (12.86) | - | - | - | 8.45 (8.89) | 12.20 (14.43) | 13.89 (20.01) | 0.98x / 1.08x / 1.20x | - |
| sumpos4 | map+reduce | 7.38 (7.52) | 8.70 (8.85) | 10.85 (14.51) | 11.51 (11.88) | - | - | - | 8.38 (8.65) | 11.23 (13.84) | 12.36 (13.61) | 0.96x / 1.04x / 1.07x | - |
| maxgap4 | prev+reduce | 3.75 (3.80) | 9.04 (9.15) | 10.66 (12.10) | 11.16 (16.07) | - | - | - | 8.74 (8.92) | 13.97 (14.23) | 11.58 (14.58) | 0.97x / 1.31x / 1.04x | - |
| scale4 | map | 6.38 (6.47) | 7.26 (7.37) | 9.16 (16.29) | 9.50 (10.72) | - | - | - | 7.04 (7.15) | 9.61 (12.01) | 10.89 (11.37) | 0.97x / 1.05x / 1.15x | - |
| filter4 | filter | 3.56 (3.67) | 4.25 (4.40) | 7.26 (8.37) | 6.83 (8.54) | 7.57 (7.92) | 9.61 (14.32) | 10.47 (11.20) | 7.05 (7.26) | 11.22 (15.06) | 10.18 (17.65) | **1.66x / 1.54x / 1.40x** | 0.93x / 1.17x / 0.97x |
| prefix4 | scan | 4.67 (4.79) | 4.73 (4.83) | 7.17 (8.98) | 8.16 (9.35) | - | - | - | 6.12 (6.42) | 8.02 (8.37) | 8.41 (13.99) | 1.29x / 1.12x / 1.03x | - |
| strip | automaton+filter | 0.90 (0.90) | 1.04 (1.05) | 0.90 (0.99) | 1.44 (1.73) | 3.97 (4.01) | 5.33 (8.45) | 6.57 (9.85) | 3.16 (3.23) | 4.52 (7.38) | 6.16 (7.74) | **3.04x / 4.35x / 4.27x** | 0.80x / 0.85x / 0.94x |
| csv | automaton+reduce | 0.85 (0.85) | 0.79 (0.80) | 2.45 (2.87) | 3.35 (3.72) | 8.23 (8.31) | 14.12 (18.27) | 10.74 (12.53) | 3.44 (3.47) | 5.43 (7.51) | 7.48 (8.44) | **4.05x / 2.22x / 2.23x** | 0.42x / 0.38x / 0.53x |
| idents | automaton+prev+reduce | 0.77 (0.79) | 0.79 (0.80) | 1.10 (1.11) | 1.24 (1.70) | 3.87 (3.91) | 8.04 (8.57) | 7.16 (13.00) | 3.24 (3.26) | 3.94 (5.52) | 4.50 (5.11) | **4.11x / 3.58x / 3.62x** | 0.84x / 0.49x / 0.56x |
| strings | automaton, prev of state | 0.88 (0.99) | 0.80 (0.81) | 0.93 (0.95) | 1.26 (1.46) | 3.95 (3.99) | 5.49 (8.62) | 6.77 (10.24) | 3.02 (3.04) | 5.17 (6.02) | 4.69 (9.97) | **3.44x / 5.56x / 3.72x** | 0.76x / 0.94x / 0.69x |
| json | automaton+table+filter | 0.55 (0.55) | 0.55 (0.56) | 0.93 (1.01) | 1.44 (1.80) | 4.81 (4.89) | 5.68 (9.37) | 10.36 (12.77) | 3.08 (3.12) | 6.77 (6.95) | 4.61 (5.99) | **5.58x / 7.29x / 3.19x** | 0.64x / 1.19x / 0.44x |

How to read it, and how not to:

- **Medians and bests differ a lot above one CPU.** This is an 8-vCPU VM; multi-thread
  timings vary by 20-50% between rounds, and a ratio of two of them more. The one-CPU
  columns are stable to a few percent. The second audit's rerun of the previous table moved
  individual 4- and 8-CPU ratios in both directions, by as much as 44% (`strings` on 4 CPUs:
  4.78x in my run, 6.87x in theirs).
- **Everything fast converges on 9-14 GB/s**, so a lead in compute shrinks as both sides
  approach whatever the rest of the process costs. I no longer attribute that to a
  measured I/O floor: the probe that was supposed to measure it was broken twice (a data
  race, then my fix let the compiler delete the reads).
- **The timing boundary** is fork to exit: process start, mapping or reading, page faults,
  computation, writes to /dev/null, exit. It is the right boundary for "run this over a
  cached file"; it is not compute-only throughput and not storage throughput.
- **Hand C is not an upper bound either.** It is one afternoon of intrinsics by the author
  of the thing it is compared with. Its `csv` and `json` use a different algorithm from
  Strand's on purpose, because that is what an expert would do.

The automaton programs and what they recognise:

```
# strip: delete '#' comments, except inside "..." strings (with \ escapes)
~256 :i 35 = i 34 = 2 * + i 10 = 3 * + i 92 = 4 * + >cls     # byte -> class: other # " newline \
0 1 2 2  1 1 2 2  2 1 0 2  0 0 2 2  0 1 3 2 =dfa             # next state, by class and state
bytes dup @cls /dfa 1 != outif                               # keep bytes whose state is not "comment"

# idents: words of [A-Za-z0-9_] that begin outside comments and strings
bytes :c @cls /dfa 0 = c @isw dup prev 1 ^ & & $N
# strings: entries into the string state
bytes @cls /dfa :s 2 = s prev 0 = & $N
# csv: separators outside quotes
bytes @cls :c /q 0 = c 2 = & $N
# json: the structural characters { } [ ] : , outside strings
~256 :i 34 = i 92 = 2 * + >cls
0 1 1 0  1 0 1 0  0 2 1 0 =dfa
~256 :j 123 = j 125 = | j 91 = | j 93 = | j 58 = | j 44 = | >st
bytes :c @cls /dfa 0 = c @st & c swap outif
```

**`strip`, `idents` and `strings` are a simplified grammar, not a lexer for any language.**
The input is the Python standard library, used as a realistic distribution of bytes; the
grammar knows `#`, `"` and `\` only. On `x = '# not a comment'` it deletes from the `#`;
it counts `123` as a word. Both sides implement the same grammar, so the timings are fair
and the outputs say nothing about Python. `csv` (quotes toggle) and `json` (strings with
escapes) are exact for valid input in their formats, and `json` is checked against a real
parser.

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

**Across threads** the carry is a function, so ranges compose: the pool's dry pass gives each
range's function, `IN[t] = OUT[t-1] o IN[t-1]`, and the real pass starts each thread in the
right state. The dry pass stops early when the automaton is the last carry on its line (it
only needs the function), which took `strip` from 3.5 to 5.1 GB/s on four CPUs.

The compiler's own source did not change for any of this: one sigil, two classes, one
stencil, table entries. The fixed point moved and still holds.

## Dependent carries: when a line does and does not run on all cores

The second audit found `input 1 & scan dup prev dup prev + + out4` running on one thread
while every test passed. Two things were behind it.

**A check that was too strict.** A `prev` carry is the previous chunk; only its last
element is ever read. Verification compared the whole vector, and after a warm-up chunk
the other elements of a `prev` of a `prev` legitimately differ. It now compares the element
that is used. Chains of `prev` are exact after one warm-up chunk.

**Carries that depend on carries.** The dry pass measures each range's effect on a carry
from a guessed starting state. If a running sum's input depends on another running sum,
or an automaton reads another automaton's states, the guess for the first poisons the
measurement of the second. The pool used to give up on the block. Now:

- The real pass still ran its *upstream* carries exactly (their guesses did not depend on
  anything). So its own start and end values give the downstream carries exactly: a sum's
  true delta is `OUT - IN`; an automaton is started not from the function so far but from
  the permutation `s -> s ^ e` (e = the state it is believed to enter in), which reaches the
  same output states and can be peeled off the result, `M = OUT o (^e)`; a running max,
  which cannot be peeled (`max(IN, M)` hides `M`), takes one extra pass from -inf.
- The pool derives again and reruns. Each retry settles one more level of dependence.
- It retries only while dry + real + retries stays below the number of threads; past that,
  one thread is as fast, and the block finishes serially as before.

So, on this machine's 8 CPUs: chains up to six carries deep run in parallel (a dependent
maximum counts as two levels), at a cost of one more pass per level. On 4 CPUs, two levels.
On 2 or 3, only lines whose carries are independent. `tests/pool.py` asserts the path, not just the
output, for 20 programs at 2, 3, 4 and 8 CPUs, using the audit's own detector (a copy of
the binary whose fallback entry exits with status 77). What remains true of the old
"cliff": the number of passes, and so the speed, depends on a property of the line that is
not visible in its source, and the verification is still the only judge of a split. I have
not benchmarked dependent-carry lines; the test establishes the path, not a speedup.

One consequence worth having: an automaton fed by another automaton's state is a cascade,
and a cascade of two 4-state automata has up to 16 joint states. That is a way to larger
automata without a new primitive. I have not yet expressed a real grammar that way.

## Which phases of a compiler fit

The question that started the project. By phase, with what is measured and what is not:

| phase | shape | in Strand today | evidence |
|---|---|---|---|
| comments and strings (which bytes are code?) | automaton | yes, up to 4 states | `strip`, `strings`: 3-5x ordinary C, 0.7-0.9x hand C. Real languages need more states: C with strings, character literals and both comment forms is 9; Python with triple quotes more. **Out until automata are wider** (or as a cascade, untested). |
| token boundaries and kinds | byte table + `prev` | yes | `idents`: 3.6-4.1x ordinary C |
| structural extraction for a real grammar (not a complete scanner: no token kinds, no validation) | automaton + table + filter | yes for JSON, CSV | `json`: 3-7x ordinary C, exact against Python's parser; 0.4-0.6x the simdjson-style hand C on 1 and 8 CPUs |
| token *positions* (an index per token) | filter of indices | **yes, since the bridge** (DESIGN.md): `m %x` on a byte line emits the positions of set lanes as dwords, and dword lines read the bytes back with `[x` / `{x`. Before it, positions existed only on dword lanes, at one dword per source byte. | the self-hosted compiler's lexer is now one automaton line; its memory fell from 2.5x the scalar C compiler's to parity |
| identifier and keyword interning | hash of a short slice, then a join | in the self-hosted compiler: four dword gathers per token, a scatter race for names, a generated perfect hash for keywords. Not a general join engine. | gather and scatter cost scalar-loop prices on this CPU (HARDWARE.md); the compiler spends most of its user time there |
| number literals | three 4-digit groups evaluated in place, scaled by length | in the self-hosted compiler (9 digits). A general segmented scan is still absent. | part of the whole-compiler figure only |
| nesting depth of brackets | prefix sum of +1/-1 outside strings | the sum yes, on dword lanes; the automaton is on byte lines, so not in one line | not built |
| matching brackets, building a tree | stack, or sort by depth | **no** | outside the model. Strand's own grammar is postfix so that it has no parser. |
| name resolution, type checking | hash join; walks over a graph | **no**, beyond the compiler's own flat symbol table | outside the model |
| dataflow optimisation | fixpoint over a graph | **no**. The compiler's own liveness is not a dataflow scan: a vector dies after its last occurrence, which the name race already identified. | outside the model |
| instruction selection and encoding | table lookup, variable-length emit | yes: this is copy-and-patch, the compiler's back end | whole compiler: 1.3-1.5x *slower* than scalar C on one core |
| layout and relocation | prefix sum, scatter | yes | same |

What this adds up to:

- **The first stage of a front end is in the sweet spot**: bytes in, classified bytes or a
  filtered byte stream out. That is also where simdjson-style parsers put their SIMD.
- **A whole compiler is not**, and the self-hosted one is the measurement. Its passes are
  expressible, but each line is a pass over arrays of dwords, symbol resolution goes through
  gather and scatter, and a scalar compiler does the same work in one cache-hot loop.
- **Lexing is a small part of what an optimising compiler does**, so even where Strand's 3x
  over ordinary C applies I would not expect it to change a build (an expectation, not a
  measurement). It matters for tools that *only* scan: dependency and include scanners,
  code search, comment and string extraction, syntax colouring, minifiers, linters' first
  pass, and ingest of JSON, CSV and logs.
- What would move the boundary next: automata of 16 states (real lexers) and a segmented
  scan. The byte-to-dword bridge that was second on this list is built.

## What does not work

- **The compiler itself** is not in the sweet spot (above, and REPORT.md).
- **Maps, reductions, neighbour tests and prefix sums are ties.** So is `swapcase`.
- **Strand does not beat hand-written intrinsics**, and `csv` shows the cost of having one
  general primitive: quote parity is a 2-state problem that carry-less multiply solves at
  8 GB/s on one core; Strand runs its 4-state automaton at 3.4.
- **Four states, sixteen classes, byte lines only.**
- **Automaton lines scale modestly**: 3 GB/s on one CPU, 4.5-7.5 on eight, and worst when a
  `prev` follows the automaton (`idents`: 3.2 to 4.5), because the dry pass cannot stop
  early and every thread runs a warm-up chunk. The hand C for `idents` reaches 7-8.
- **Deep chains of dependent carries** still fall back, by thread count (above).
- **Outside the model entirely**: histograms and anything else needing scatter-add;
  data-dependent control flow (nested structure, early exit); pointer chasing; joins.
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
- **`swapcase`, the same mistake again.** After the first audit I rewrote `words` and did
  not ask the same question of the next table row. The fix this time is structural: a hand
  C column, so the claim is a bracket instead of an argument about how strong the C is.
- **I broke the I/O probe while fixing its data race**: the per-thread sums were never
  read, so the compiler deleted the loads being timed. A measurement whose result is
  unused is not a measurement; the probe now prints a checksum.
- **The hand C for `json` was wrong on four CPUs** (a range that ends on a pending escape
  was counted as flipping string state). The per-configuration output check stopped the
  run at the last program; I fixed the C and timed that row separately.
- **Earlier ones** (git history has the detail): filters first lost by 2x until `outif`
  streamed them; the first parallel `prev` stored its warm-up chunk; the main thread's
  stack was misaligned inside the pool; a fully masked store to unmapped memory took a
  microcode assist per chunk.

## Where this points

- **Parity as a primitive.** A 2-state toggle is a prefix xor, one carry-less multiply per
  64 bytes. `csv` and most of `json` would run near the hand C's speed.
- **16-state automata** (real lexers, UTF-8 validation): 16-byte functions in 128-bit
  lanes. Or cascades of the 4-state ones, which now run in parallel; try a real grammar.
- **A byte-to-dword bridge**, so a line can turn "this byte starts a token" into a list of
  positions.
- **Segmented scans** (a sum that resets at each newline or token) and **number parsing**
  (a scan over affine maps).
