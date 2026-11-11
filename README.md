# Strand

A language whose source has no loops and no branches, and a compiler for it — written in
it — that runs as AVX-512 code in every phase and reproduces itself byte for byte.
(The generated machine code and the runtime do contain loops and branches; SCALAR_LEDGER.md
counts them. About 60% of the instructions the compiler executes are vector instructions.)

```
input >s                          # read stdin
s 97 >= s 122 <= & >islow         # mask: is this byte a lowercase letter?
s islow 32 * - out                # subtract 32 where the mask is set; write stdout
```

Every line is one fused SIMD loop over `i32` lanes (or 64 byte lanes). Lines are postfix;
the stack depth of a value is the zmm register it lives in. The only state a line can
carry is associative — previous element, running sum, running max, a 4-state automaton —
so every line vectorises, and a line whose carries do not feed each other more than a few
levels deep is split across cores (deeper chains run on one; DESIGN.md says exactly when):

```
~256 :i 35 = i 34 = 2 * + i 10 = 3 * + i 92 = 4 * + >cls   # byte -> class: other # " newline \
0 1 2 2  1 1 2 2  2 1 0 2  0 0 2 2  0 1 3 2 =dfa           # next state: code, comment, string, escape
bytes dup @cls /dfa 1 != outif                             # drop '#' comments outside "..." strings: 3 GB/s on one core
```

| read this | for |
|---|---|
| [AUDIT.md](AUDIT.md) | current audit verdict, investigation backlog, and original correctness/benchmark review |
| [AUDIT_FOLLOWUP.md](AUDIT_FOLLOWUP.md) | detailed review of the audit response and new automata, with fresh measurements and counterexamples |
| [REPORT.md](REPORT.md) | what is new, what was measured, what failed, what next |
| [GOLEX.md](GOLEX.md) | a Go lexer in Strand, token for token equal to `go/scanner` on the standard library: 531 ms on one core, 180 ms on eight, against 601 ms for `go/scanner`'s scan |
| [LINKING.md](LINKING.md) | what shape linking has, and a linker written in Strand |
| [SWEETSPOT.md](SWEETSPOT.md) | where this beats ordinary C (filters, table lookups, small automata), where it only ties (everything GCC vectorises, and hand-written intrinsics), which compiler phases fit, and why |
| [DESIGN.md](DESIGN.md) | the language and why each compiler phase disappears |
| [HARDWARE.md](HARDWARE.md) | what this machine's SIMD really executes and costs |
| [SCALAR_LEDGER.md](SCALAR_LEDGER.md) | every place the "all SIMD" claim is not true, with numbers |
| [REVIEWS.md](REVIEWS.md) | three rounds of Codex review and the audit, and what was done about each point |
| [blog/](blog/) | the checkpoint posts |

## Build and test

Needs an x86-64 CPU with AVX-512 F/BW/DQ/VL/CD/VBMI/VBMI2 + BMI2 + POPCNT (there is no
runtime dispatch or fallback), Linux, gcc with OpenMP (for the C baselines), binutils, Python 3.

```
./build.sh        # stencils -> stage0 (Python) -> stage1 -> stage2 -> stage3; fails unless all equal
./test.sh         # everything, on every stage (about 10 minutes)
tests/mutation.sh # inject five bugs, check the suite catches each
```

```
./build/stage2 < examples/upper.strand > upper && chmod +x upper && echo hello | ./upper
printf 'bytes 32 > dup prev 1 ^ & $W\n~1 W + out4\n' | ./build/stage2 > words && chmod +x words
./words < bigfile | od -An -tu4          # word count, ~6.5 GB/s on one core, all cores if the file is big
python3 bench/run.py 256                 # the SWEETSPOT.md table
```

## Layout

| path | what |
|---|---|
| `src/compiler.strand` | the compiler, in Strand (~155 lines) |
| `src/tables.strand` | generated: stencil bytes and lookup tables, as Strand data lines |
| `boot/spec.py` | the constants every implementation must agree on, incl. the name race |
| `boot/strand.py` | scalar reference compiler (stage 0) and interpreter |
| `boot/strandc.c` | the same compiler in plain C: speed baseline and second oracle |
| `tools/mkstencils.py` | one stencil per (operation, stack depth), assembled by GNU as |
| `stencils/stub.S` | startup, I/O and the thread pool: the hand-written scalar part |
| `bench/` | 21 programs with plain-C and mmap+OpenMP C baselines; `run.py` checks every timed configuration's output |
| `explore/` | probes behind SWEETSPOT.md: ceiling, I/O cost, `fsm.c` (automaton prototype), `cliff.c` (five spellings of one loop) |
| `tests/` | differential fuzzers (semantics, compiler bytes), hand cases, mutation check |
| `tools/prof.c`, `profreport.py`, `liveset.py`, `lint.py`, `where.py` | ptrace profiler (no perf/gdb on the VM), allocator model, error locators |
| `hw/probe.c`, `hw/check_probe.py` | the hardware probe behind HARDWARE.md, and a check that each probe contains its instruction |
