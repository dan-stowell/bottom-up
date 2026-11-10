# Strand

A language with no loops and no branches, and a compiler for it — written in it — that
runs as AVX-512 code in every phase and reproduces itself byte for byte.

```
input >s                          # read stdin
s 97 >= s 122 <= & >islow         # mask: is this byte a lowercase letter?
s islow 32 * - out                # subtract 32 where the mask is set; write stdout
```

Every line is one fused SIMD loop over `i32` lanes. Lines are postfix; the stack depth of
a value is the zmm register it lives in. There is nothing else in the language.

| read this | for |
|---|---|
| [AUDIT.md](AUDIT.md) | correctness and benchmark reproduction audit (2026-11-10), including counterexamples |
| [REPORT.md](REPORT.md) | what is new, what was measured, what failed, what next |
| [SWEETSPOT.md](SWEETSPOT.md) | where this model beats C by 5-12x, where it does not, and the I/O floor |
| [DESIGN.md](DESIGN.md) | the language and why each compiler phase disappears |
| [HARDWARE.md](HARDWARE.md) | what this machine's SIMD really executes and costs |
| [SCALAR_LEDGER.md](SCALAR_LEDGER.md) | every place the "all SIMD" claim is not true, with numbers |
| [REVIEWS.md](REVIEWS.md) | two rounds of Codex review, and what was done about each point |
| [blog/](blog/) | the two checkpoint posts |

## Build and test

Needs an x86-64 CPU with AVX-512 F/BW/DQ/VL/CD + BMI2, Linux, gcc, binutils, Python 3.

```
./build.sh        # stencils -> stage0 (Python) -> stage1 -> stage2 -> stage3; fails unless all equal
./test.sh         # everything, on every stage (about 10 minutes)
tests/mutation.sh # inject five bugs, check the suite catches each
```

```
./build/stage2 < examples/upper.strand > upper && chmod +x upper && echo hello | ./upper
printf 'bytes 32 > dup prev 1 ^ & $W\n~1 W + out4\n' | ./build/stage2 > words && chmod +x words
./words < bigfile | od -An -tu4          # word count, ~8 GB/s on one core, all cores if the file is big
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
| `bench/` | 17 sweet-spot programs with scalar-C and gcc -O3 baselines |
| `explore/` | ceiling and I/O-floor probes behind SWEETSPOT.md |
| `tests/` | differential fuzzers (semantics, compiler bytes), hand cases, mutation check |
| `tools/prof.c`, `profreport.py`, `liveset.py`, `lint.py`, `where.py` | ptrace profiler (no perf/gdb on the VM), allocator model, error locators |
| `hw/probe.c` | the hardware probe behind HARDWARE.md |
