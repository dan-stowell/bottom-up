# DESIGN.md — Strand

*Written before reading any outside review (see REVIEWS.md for what changed afterwards).*

Strand is a language designed backwards from one question: **what would a language have to
look like for its own compiler to contain no loops and no branches?**

## The bet

A Strand program is a straight-line list of lines. There is no `if`, no `while`, no
recursion, no functions. Every line is one fused SIMD loop over a flat array of `i32`.
So a compiler written in Strand is *by construction* a fixed sequence of data-parallel
passes: the only loops that exist are the ones the code generator wraps around each line,
and the only branches are loop back-edges and runtime safety checks.

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
| `!abc` | sink: `vals idx !abc` does `abc[idx] = vals` in place (scatter, last writer wins) |
| `out`, `out4` | sink: append to stdout as bytes / as little-endian dwords |
| `halt` | sink (scalar line): if nonzero, exit with that status and no output |
| `1 2 "txt" =abc` | data line: `abc` is a constant vector (numbers, string bytes) |

## What the design makes disappear

| classical phase | why it is sequential | what Strand does instead |
|---|---|---|
| Lexing | state machine | Token = run of non-blanks. Boundaries are `blank != prev(blank)`. Comments and strings are a prefix count compared against its value at line start. |
| Parsing | stack / recursion | **There is no tree.** Lines are postfix; nothing nests across tokens. |
| Precedence | Pratt/shunting-yard | None. Postfix. |
| Register allocation | graph colouring / linear scan | **Stack depth is the register number.** `depth = scan(delta[kind])`. A token at depth d works on `zmm(d-1)`, `zmm(d)`. One prefix sum allocates every register in the program. Because every line must end at depth 0, the scan never needs resetting per line. |
| Type checking / inference | dataflow in dependency order | **Case is the type.** `abc` is a vector, `Abc` a scalar; the sink sigil gives the line's shape. Each token is typed by looking at itself. |
| Symbol table | hash map built sequentially | **Scatter race** (below). |
| Keyword recognition | trie / perfect hash | **Self-join with a prelude.** The compiler prepends a line listing every operator name. Operators are then ordinary names, and "is a keyword" means "its canonical occurrence lies in the prelude". No keyword table, no string compare. |
| Instruction selection | tree pattern matching | One token = one stencil, selected by `(kind, depth, line shape)`. |
| Instruction encoding | variable-length, operand-dependent | **Copy-and-patch as gather + scatter.** Stencils are pre-assembled for every (op, depth). Sizes are prefix-summed to offsets; every output byte gathers its stencil byte; operand displacements are scattered in. |
| Branch offsets | fixups in address order | Loop back-edges jump through a register (`jmp r11`) loaded at loop head. No relative branch ever crosses a stencil. Nothing to fix up. |
| Layout / linking | back-patching headers | Output is a *concatenation* (`out` appends), so sizes are emitted where they belong instead of patched in later. |

### Scatter-race symbol resolution

Every name token computes a 32-bit polynomial hash (segmented sum = difference of two
prefix-sum values). Then, with no sorting and no sequential insertion:

1. every token scatters its own index into `table[h mod M]` — AVX-512 scatter semantics
   make the *last* writer win, deterministically;
2. every token gathers the slot back and asks: "is the winner's hash mine?" If yes, the
   winner is the canonical occurrence of my name. All occurrences of one name agree,
   because they collide with each other on purpose;
3. names that lost to a *different* name race again in a second table under a different
   hash, then a third.
4. Finally every byte of every name is compared against the corresponding byte of its
   canonical occurrence (one gather), so a 32-bit hash collision is a detected compile
   error, never a miscompile.

A name's slot number is then the running count of canonical occurrences.

## Code shape

One line `a b + 3 * >c` becomes:

```
lead    : load a's length, bump-allocate the output, zero the carries, i = 0
loop    : k7 = active-lane mask for this chunk
          zmm0 = a[i..]                      ; push a        depth 1
          zmm1 = b[i..]  (length checked)    ; push b        depth 2
          zmm0 = zmm0 + zmm1                 ; +             depth 1
          zmm1 = broadcast(3)                ; push 3        depth 2
          zmm0 = zmm0 * zmm1                 ; *             depth 1
          out[i..] = zmm0 ; i += 16 ; loop   ; >c            depth 0
          c = (out, n)
```

Scans, gathers, compress-stores and scatters are stencils *inside* the same loop — there
is no runtime library of kernels. Everything the compiler executes outside a small
startup/exit stub is code its own stencil-gather produced.

## Bootstrap

- **Stage 0** is a deliberately naive scalar compiler in Python (`boot/`): dictionaries,
  for-loops, lists. It is the *reference*: the SIMD compiler must reproduce its output
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

## Known costs accepted up front

- `i32` only: bytes are widened 4x. HARDWARE.md says this spills L1 at 8 KiB of source and
  that byte lanes would be 4x denser. Chosen anyway for a one-type language; revisit.
- Gather/scatter cost scalar-loop prices on this CPU. The design leans on them (symbol
  race, stencil gather). They are branch-free and fuse into the loop, but they are not a
  speedup, and the ledger will say so with numbers.
- No reuse of memory: every line bump-allocates its result.
