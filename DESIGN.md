# DESIGN.md — Strand

*The first version of this file was written before reading any outside review (git history has
it). This version describes the language as built; REVIEWS.md records what changed and why.*

Strand is a language designed backwards from one question: **what would a language have to
look like for its own compiler to contain no loops and no branches?**

## The bet

A Strand program is a straight-line list of lines. There is no `if`, no `while`, no
recursion, no functions. Every line is one fused SIMD loop over a flat array of `i32`
(or, on a byte line, of bytes). "No loops and no branches" describes the *source language*;
the generated code and the runtime have both, counted in SCALAR_LEDGER.md.
So a compiler written in Strand is *by construction* a fixed sequence of data-parallel
passes: the only loops that exist are the ones the code generator wraps around each line.
(Branches do exist in the *generated* code: loop back-edges, runtime safety checks, the
allocator's free-list-or-bump choice, and `halt`. They are per line, never per element.
SCALAR_LEDGER.md counts them.)

That means the compiler's language must be compilable by passes that are each one
scan / elementwise map / compress / gather / scatter. Wherever the classical algorithm is
sequential, the language is bent until the problem is gone.

## The language

```
# count bytes greater than 64, write the count as one byte
input 64 > $N        # vector line: lead `input`, ops, sink `$N` (sum into scalar N)
~1 N + out           # vector line of length 1: iota, + N, write to stdout
```

- A **token** is a maximal run of non-blank bytes. A **line** is one statement.
- Lines are **postfix**. Values are pushed, operators consume the top of the stack, and the
  last token of the line is a **sink** that consumes the result.
- All values are `i32`. Vectors are named in lowercase, scalars in Uppercase. Truth is 0/1.

| token | meaning |
|---|---|
| `abc` | push vector variable |
| `Abc` | push scalar variable (broadcast) |
| `123` | push literal |
| `&abc` | push length of vector `abc` |
| `@abc` | gather: replace top-of-stack `i` with `abc[i]` (bounds-checked) |
| `~N` | lead only: line has length `N`; pushes `0,1,..,N-1` |
| `input` | lead only: line has the length of stdin; pushes its bytes |
| `+ - * & \| ^ min max << >> < <= > >= = != ` | elementwise binary |
| `sel` | `c a b sel` = c ? a : b |
| `scan`, `scanmax` | running sum / running max (inclusive) |
| `prev` | value at the previous position (0 at position 0) |
| `dup drop swap` | stack shuffles |
| `>abc` | sink: store vector |
| `>Abc` | sink: store scalar (scalar line) |
| `$Abc`, `^Abc` | sink: sum / max of the line into a scalar |
| `?abc` | sink: `vals mask ?abc` stores the `vals` whose mask is nonzero (compress) |
| `!abc` | sink: `vals idx !abc` does `abc[idx] = vals` in place (scatter, last writer wins; a negative index is a masked-off lane) |
| `:abc` | tee: store the top of stack as vector `abc` and *keep going* (one loop, many outputs) |
| `%abc` | tee: `vals mask %abc` compress-stores mid-line |
| `idx` | push the lane index `0,1,2,...` |
| `end` | sink: discard the result (the line's work was its tees) |
| `input4` | lead only: stdin as little-endian 32-bit integers |
| `bytes` | lead only: stdin as bytes on **64 unsigned byte lanes** — a *byte line* (below) |
| `/abc` | byte lines only: run the 4-state automaton `abc` over the line; each element becomes the state after reading it (below) |
| `outif`, `outif4` | sink: `vals mask outif` appends the selected values to stdout (a streaming filter) |
| `out`, `out4` | sink: append to stdout as bytes / as little-endian dwords |
| `halt` | sink (scalar line): if nonzero, exit with that status at once. Output is streamed, so whatever earlier lines wrote may already be on stdout |
| `1 2 "txt" =abc` | data line: `abc` is a constant vector (numbers, string bytes) |

### Byte lines

A line led by `bytes` runs with 64 byte lanes per chunk instead of 16 dword lanes. All
arithmetic is mod 256, comparisons are unsigned, `@t` is a 256-entry table lookup (two
`vpermi2b`), and the sinks are the streaming ones (`out`, `outif`, `$S`, `end`). Named
vectors do not exist on a byte line except as chunk-local tees. In the compiler this is one
table: a token on a byte line takes class `BYTEOF[class]`, and classes with no byte meaning
have no stencil, so misuse is the ordinary "no stencil at this depth" error.

### Automata

`x /t` is the one way to carry *state* along a line. `t` is a table of next states:
`state' = t[(x & 15) * 4 + state] & 3`, four states, up to sixteen input classes, starting
in state 0; entries past the end of `t` are 0. The result is the state after each element.

```
~256 :i 35 = i 34 = 2 * + i 10 = 3 * + i 92 = 4 * + >cls     # byte -> class: other # " newline \
0 1 2 2  1 1 2 2  2 1 0 2  0 0 2 2  0 1 3 2 =dfa             # states: code, comment, string, escape
bytes dup @cls /dfa 1 != outif                               # strip comments, respecting strings
```

A state machine is the textbook sequential loop. Here it is a *scan of transition
functions*: a function on four states is four bytes, so it fits a dword lane and composing
two is one `vpshufb`; the chunk's 64 functions are composed by a short chain and a
Kogge-Stone scan, in registers, with no loop (SWEETSPOT.md has the layout). Composition is
associative, so thread ranges combine the same way running sums do. Like every other
carry in the language, it is a scan over a small monoid; that is the property the whole
design leans on.

### From bytes to tokens: the bridge

A byte line can leave two kinds of vector behind, and a dword line can read both:

```
bytes :text dup 32 > dup prev 1 ^ & %start end   # keep the bytes; record where words begin
start [text out                                  # first byte of every word
start {text out4                                 # first four bytes of every word, as one dword
```

- `>x` (or the tee `:x`) on a byte line stores the bytes as a **byte vector**.
- `m %x` on a byte line appends the **positions** of the nonzero lanes of `m` to the dword
  vector `x`. This is what turns "this byte starts a token" into a token stream.
- `i [x` on a dword line is the byte of `x` at index `i`; `i {x` is the little-endian dword
  starting there, with bytes past the end of `x` read as 0. An index outside the vector is a
  runtime failure, as for `@x`.
- A name is a byte vector or a dword vector for the whole program. Using one as the other is
  a compile error (`E_TYPE`, 14), checked the same way as every other rule: each typed use
  scatters its type to the name's slot and every use compares.

`%x` keeps its line on one thread, like the dword `%` tee.

### Building bytes: slices and dword stores

What a code generator and a linker do is concatenate pieces and patch them. Four forms:

```
sten .stenb                                  # the low byte of every lane, as a byte vector
~C 0 * .cb                                   # C zero bytes
tid dup @qstart psz offp (stenb )cb ... end  # one slice per token: psz bytes from stenb[qstart..] to cb[offp..]
value position }cb                           # a dword at a byte position
'cb out                                      # a byte line over a byte vector
```

- `v .x` (sink) stores the low byte of each lane as the byte vector `x`; `'x` (lead) starts
  a byte line over a byte vector, as `bytes` does over stdin.
- `s n d (src )dst` copies `n` bytes from `src[s..]` to `dst[d..]`, one slice per lane, in
  lane order. `(src` names the source for its line; `)dst` pops the three operands and the
  line goes on, so one pass can place several slices per element. A slice that leaves
  either vector is a runtime failure.
- `v p }x` (sink) stores the dword `v` at byte offset `p` of `x`. A negative `p` is a
  masked-off lane.

`}x` is one `vpscatterdd`. `)dst` is not a vector operation: the destination of each slice
is data, so the stencil walks the 16 lanes one at a time and moves each slice with masked
64-byte loads and stores (SCALAR_LEDGER.md, item 26). The compiler's own code emission is
now these two: one line copies every token's three stencil slices, three lines store the
operands. Before, every code byte was a dword that gathered its stencil byte, and every
operand was four byte scatters.

### Failure and output

A runtime check that fails exits with status 99, `halt` with its own status. Output is
streamed in 256 KiB pieces, so a failing program may already have written part of its
output. The interpreter returns no output on failure and the tests compare only the status
in that case; the streaming behaviour is the definition (AUDIT.md, defect 5).

### What runs on all cores

Nothing in the language says "parallel". At run time the lead token's prologue marks its
line splittable and any token that cannot be split (compress into memory, scatter) vetoes
in its own prologue. A big splittable line calls a thread pool in the startup stub; the
workers jump into the *same* loop body with their own index range. `prev` is made right by
a one-chunk warm-up that stores nothing; running sums, maxima and automata by a dry pass,
derived carries, a real pass and a verification. A carry that depends on another carry (a
sum of a sum, an automaton fed by an automaton's states) is guessed wrong by the dry pass;
the real pass then ran the carries it depends on exactly, so its own start and end values
give the right carries for one more pass. The pool retries like that while the number of
passes stays below the number of threads, and otherwise finishes the block on one thread
(SWEETSPOT.md, "Dependent carries"). Parallel execution is therefore a property of the
line *and* the thread count, not a guarantee: `tests/pool.py` asserts which path each of
its programs takes. The compiler's source did not change for any of this.

## What the design makes disappear

| classical phase | why it is sequential | what Strand does instead |
|---|---|---|
| Lexing | state machine | Token = run of non-blanks. Boundaries are `blank != prev(blank)`. Comments and strings are a prefix count compared against its value at line start. |
| Parsing | stack / recursion | **There is no tree.** Lines are postfix; nothing nests across tokens. |
| Precedence | Pratt/shunting-yard | None. Postfix. |
| Register allocation | graph colouring / linear scan | **Stack depth is the register number.** `depth = scan(delta[kind])`. A token at depth d works on `zmm(d-1)`, `zmm(d)`. One prefix sum allocates every register in the program. Because every line must end at depth 0, the scan never needs resetting per line. |
| Type checking / inference | dataflow in dependency order | **Case is the type.** `abc` is a vector, `Abc` a scalar; the sink sigil gives the line's shape. Each token is typed by looking at itself. |
| Symbol table | hash map built sequentially | **Scatter race** (below). |
| Lexing | a character loop with a state variable | **One automaton on byte lanes**, then positions. `text @lcls /ldfa` gives every byte's state (code, comment, string); token starts and ends are where "token character" changes; `%` hands their positions to dword lines. Nothing per byte is kept but the byte. |
| Token text (names, numbers) | pointer + length, read byte by byte | **Four dwords per token.** A name has at most 16 bytes and a literal 9 digits, so four gathers from a token's first byte hold all of it. Equality is four compares; a decimal value is three 4-digit groups evaluated in place. |
| Keyword recognition | trie / perfect hash | A perfect hash, generated with the tables: one lookup by the name's fingerprint, confirmed by comparing the four dwords. (Until the front end moved to byte lanes this was a self-join with a prelude line of operator names; the join cost a pass over the bytes.) |
| Instruction selection | tree pattern matching | One token = one stencil, selected by `(kind, depth, line shape)`. |
| Instruction encoding | variable-length, operand-dependent | **Copy-and-patch as gather + scatter.** Stencils are pre-assembled for every (op, depth). Sizes are prefix-summed to offsets; every output byte gathers its stencil byte; operand displacements are scattered in. |
| Branch offsets | fixups in address order | Loop back-edges jump through a register (`jmp r11`) loaded at loop head. No relative branch ever crosses a stencil. Nothing to fix up. |
| Layout / linking | back-patching headers | Output is a *concatenation* (`out` appends), so sizes are emitted where they belong instead of patched in later. |

### Scatter-race symbol resolution

Every name token computes a 64-bit fingerprint: two sums, over the name's four dwords, of a
mixed (dword + position) value. Then, with no sorting and no sequential insertion:

1. every token scatters its own index into `table[key]` — AVX-512 scatter semantics make
   the *last* writer win, deterministically;
2. every token gathers the slot back and asks: "does the winner have my fingerprint?" If
   yes, the winner is the canonical occurrence of my name — its last occurrence. All
   occurrences of one name agree, because they collide with each other on purpose;
3. names that lost to a *different* name race again under the next round's key (4 rounds);
4. finally the four dwords of every name are compared with those of its canonical
   occurrence (four gathers), so a fingerprint collision is a detected compile error, never
   a miscompile.

A name's slot number is then the running count of canonical occurrences.

The race is not complete: a name can lose four times (E_RACE), and two names can share a
64-bit fingerprint (E_COLLISION). Rather than pretend otherwise, **the race is part of the
language definition** (`boot/spec.py`): the table is `2 x pow2ceil(tokens)` slots, the
fingerprint and round keys are fixed, and the scalar references simulate the same race with
plain loops, so every implementation accepts exactly the same programs. Codex found inputs
that broke the first, unspecified version (REVIEWS.md, review 2).

### Liveness and hoisting, also by scan

Two things a compiler normally needs an analysis for fall out of the same structures:

- **Liveness.** With no loops, a vector is dead after the line containing its last
  occurrence — and the race makes exactly that token the canonical one (`rep == self`).
  The compiler emits a *release* after that line; stores release what they overwrite.
  Blocks are recycled through size-class free lists.
- **Loop-invariant hoisting.** Every token may have a *prologue* stencil (length check,
  allocation, carry init) besides its body. A line is laid out as
  `[all prologues][all bodies][releases]`. The three streams are sized, prefix-summed and
  interleaved by arithmetic on the sums; nothing is moved.

## Code shape

One line `a b + 3 * >c` becomes:

```
prologues: n = len(a); zmm27 = n; zmm26 = lane indices     ; a   (lead)
          check len(b) == n                                ; b   (hoisted)
          out = allocate(n)                                ; >c  (hoisted)
loop    : k7 = (lane index < n)             one vpcmpud
          zmm0 = a[i..]                      ; push a        depth 1
          zmm1 = b[i..]                      ; push b        depth 2
          zmm0 = zmm0 + zmm1                 ; +             depth 1
          zmm1 = broadcast(3)                ; push 3        depth 2
          zmm0 = zmm0 * zmm1                 ; *             depth 1
          out[i..] = zmm0 ; i += 16 ; loop   ; >c            depth 0
          release c's old block; c = (out, n)
releases: free any vector whose last occurrence was on this line
```

Scans, gathers, compress-stores, scatters and the allocator are stencils *inside* the
line — there is no runtime library of kernels. Everything the compiler executes outside a
small startup/exit stub was placed by its own stencil gather. "Code generator" here means
selection, layout, copying and operand patching; the instruction *encodings* inside each
stencil come from GNU as at build time (SCALAR_LEDGER.md, item 8).

## Bootstrap

- **Stage 0** is a deliberately naive scalar compiler in Python (`boot/strand.py`): dictionaries,
  for-loops, lists. `boot/strandc.c` is the same compiler again in plain C: the speed
  baseline and a second, independently written oracle. It is the *reference*: the SIMD compiler must reproduce its output
  byte for byte on every input. Python because the only job of stage 0 is to be obviously
  right and to be thrown away; nothing of it survives into the fixed point.
- A scalar **interpreter** in Python defines what programs *mean*, independently of any
  code generation. Compiled programs are differentially tested against it.
- Stencils are assembled once by GNU `as` from `stencils/*.S` and become data lines in the
  compiler's own source. This is the one place external tooling leaks in; it is recorded
  in SCALAR_LEDGER.md.
- **Stage 1** = stage0(compiler.strand). **Stage 2** = stage1(compiler.strand).
  **Stage 3** = stage2(compiler.strand). Fixed point: stage2 == stage3. Since stage 0 and
  the Strand compiler implement the same function, stage1 == stage2 is also expected and
  is a stronger check.

## Known costs accepted up front (and what measuring them showed)

- `i32` only: bytes are widened 4x. This turned out to be the dominant cost, but through
  page faults rather than cache misses (REPORT.md).
- Gather/scatter cost scalar-loop prices on this CPU. They are branch-free and fuse into
  the loop, but they are not a speedup.
- Memory was originally never reused; that was 80% of compile time and is fixed (above).
