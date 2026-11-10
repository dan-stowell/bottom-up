# After the audit

*Third checkpoint. The first two posts were written by the person who built the thing.
This one is written after someone else checked it.*

## What broke

The claim was: wherever a loop needs its neighbour, C compilers give up and Strand is 3-4x
ahead of parallel C. The evidence was a word counter. The C baseline looked like this:

```c
for (size_t i = 0; i < n; i++)
  acc += (in[i] > 32) & (i == 0 || in[i - 1] <= 32);
```

The auditor moved `i == 0` out of the loop. GCC vectorised it. 1.5 GB/s became 7, and the
lead became 1.1x with overlapping error bars. I had measured a slow loop and explained it
with a statement about compilers that I never tried to falsify.

Afterwards I wrote the loop five ways (`explore/cliff.c`). One vectorises:

```
carry a flag                 1.38 GB/s
if/else state machine        1.59 GB/s
look back, guard i == 0      1.84 GB/s
look back, peeled           19.62 GB/s
carry the previous byte      1.46 GB/s
```

So the honest version of the old claim is not "C cannot" but "C can, if you find the one
spelling in five". That is a real property of auto-vectorisation and a real advantage of a
language where the slow spellings do not exist. It is not a 4x speed-up.

## What is left

With baselines rebuilt to be as strong as I can make them, most of the table is a tie.
Three classes are not: table translation, filters, and anything with *state*.

State was not in the language. A state machine is the loop everybody agrees is sequential,
and Strand had no loops, so there was nothing to write. But the same trick that makes a
running sum fit in a register works for an automaton, if you scan the right thing:

- A transition function on four states is four bytes. It fits one 32-bit lane.
- Composing two of them is a byte shuffle: one `vpshufb`.
- Composition is associative. So 64 bytes' worth of functions can be combined by a prefix
  scan inside registers, and the ranges given to different threads can be combined the
  same way afterwards.

```
0 1 2 2  1 1 2 2  2 1 0 2  0 0 2 2  0 1 3 2 =dfa     # code, comment, string, escape
bytes dup @cls /dfa 1 != outif                       # strip comments, respecting strings
```

That line runs at 3.1 GB/s on one core. The C `switch` loop runs at 0.96, and a C compiler
will neither vectorise it nor split it across threads; the hand-threaded C version (run
every range from all four states, then again from the right one) reaches 1.5 on eight
threads, half of what the Strand line does on one. Counting identifiers outside comments
and strings: 3.2 against 0.78.

The first version of the stencil did 99 vector operations per 64 bytes. Laying the
functions out transposed — register *t* holds bytes 4g+t — turns the first two levels of
the scan into a three-step chain across registers and brings it to 36. On this CPU, where
a 512-bit operation costs two 256-bit ones, that was the difference between 3 and 6 GB/s
in the prototype.

Nothing in the compiler's own source changed: a sigil, two classes, a stencil, table
entries. The fixed point moved and held.

## What I think the project is now

Not "a faster way to write loops". For loops a compiler already understands, it is a tie.

The property that matters is that the only carried state the language admits is
associative: previous element, running sum, running max, automaton. A compiler given a C
loop has to *discover* that; a Strand line has it by construction, which is why every line
vectorises and every line can be split, with no analysis and a compiler that runs in a
quarter of a millisecond. The class of programs where that buys speed is byte streams with
a little state: tokenise, strip, classify, filter, translate. It is 3-5x there, not 15x,
and I would rather have the smaller number that someone else can reproduce.

---

*Added after a second audit, the same day.* Two things in this post needed qualifying. The
grammar above knows `#`, `"` and `\` and nothing else; it is not a lexer for Python or any
other language, and the input was Python source only as a realistic mix of bytes. And "a C
compiler will neither vectorise it nor split it" is a statement about GCC and an ordinary
loop: C written by hand with intrinsics, using this same technique, is somewhat faster than
the Strand line (4.0 against 3.2 GB/s on one core). The next post has that comparison.
