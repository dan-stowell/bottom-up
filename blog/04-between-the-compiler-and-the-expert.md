# Between the compiler and the expert

*2026-11-10. Third post. The second audit of this project's claims found that my "strongest
C" baseline had, again, a loop that was slow because of how I wrote it. This post is about
what replaced the argument.*

## The same mistake, one row down

The first audit showed that my word-count loop in C was slow because of its spelling, and I
rewrote it. The next row of the table translated bytes through a 256-entry table,
`out[i] = tab[in[i]]`, to swap upper and lower case. GCC does not vectorise that, Strand
does, and I reported a 2x lead for "table lookups".

The auditor replaced the lookup with the arithmetic it stood for:

```c
int u = c & 223;
out[i] = c ^ (((u >= 65) & (u <= 90)) * 32);
```

GCC vectorises this, and the lead went from 2.3x to 1.1x on one core. I had asked "can
another spelling vectorise?" of exactly one loop, the one I had been caught on.

## A bracket instead of an argument

I could keep hardening the C one audit at a time. Instead there are now three
implementations of every program where ordinary C loses:

- ordinary C, shaped so GCC vectorises what it can, with OpenMP;
- **C with AVX-512 intrinsics and OpenMP, written by hand** to do what the Strand stencils
  do, or something better where I know something better;
- the Strand line.

On 256 MiB, one core, GB/s:

| program | ordinary C | hand C | Strand |
|---|---|---|---|
| translate bytes through a table (EBCDIC to Latin-1) | 2.4 | 6.0 | 5.5 |
| keep the lowercase letters | 1.5 | 6.0 | 5.6 |
| keep the positive integers | 4.3 | 7.6 | 7.1 |
| drop `#` comments outside `"..."` strings | 1.0 | 4.0 | 3.2 |
| JSON: structural characters outside strings | 0.55 | 4.8 | 3.1 |
| CSV: separators outside quotes | 0.85 | 8.2 | 3.4 |

So the claim has two sides now. Against a compiler given an ordinary loop, Strand is 1.4 to
7 times faster on these shapes. Against an expert it is behind: 75-95% of the speed when
the expert uses the same technique, and less than half for CSV, where the expert does not
use an automaton at all. Quote parity is a prefix xor, and a carry-less multiply computes
64 of them in one instruction. Strand has one general primitive for state machines and
pays for that generality on the two-state ones.

What Strand is, then: a compiler that gets most of the way to hand-written intrinsics from
one line, in a quarter of a millisecond, with the threading included. The hand-written
JSON scanner is about 30 lines, and its threading had a bug (a range ending on a pending escape)
that only showed on four CPUs. The benchmark's output check caught it.

## The fallback nobody saw

The audit also found a line that ran on one thread while every test passed. The cause was a
check comparing 16 lanes of a carry of which the program reads one. Fixing that led to the
general case: a running sum whose input depends on another running sum, or an automaton
reading another automaton's states. The thread pool guesses the first, so its measurement
of the second is wrong, and it used to give up.

It now retries. The pass that failed still ran the first carry exactly, so its own start
and end values give the second carry exactly. For an automaton this needs a trick: each
thread starts not from "the function so far" but from the permutation that sends state 0 to
the state it is believed to be in. The outputs are the same, and a permutation can be
undone afterwards to recover the range's own function. Each retry settles one more level,
and the pool stops retrying when the passes would outnumber the threads.

The tests now assert which path ran, using the auditor's detector.

## Does any of a compiler fit?

The scanning end does: which bytes are code, where tokens begin, the structure of JSON.
That is 3-7x ordinary C today for grammars of up to four states. Real programming
languages need more (C's comments, strings and character literals are nine), there is no
way yet to emit token *positions* from a byte line, and everything after the lexer
(matching brackets, names, types, dataflow) is outside the model. The self-hosted compiler
remains the honest measurement of the whole: slower than scalar C.

SWEETSPOT.md has the phase-by-phase table and the full benchmark.
