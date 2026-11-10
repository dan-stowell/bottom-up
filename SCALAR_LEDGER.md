# SCALAR_LEDGER.md

Every place where "everything is data-parallel" is not true, what it costs, and why it is
still there. Measured on stage2 (sha256 `2736fc84…`) compiling its own source — 44.7 KB,
13,759 tokens, 117,000 output bytes — on the machine in HARDWARE.md.

How it was measured (this VM has no `perf` or `gdb`):
- **instruction counts**: `tools/prof.c count`, ptrace single-step, exact (4,496,131 instructions);
- **time shares**: `tools/prof.c sample`, SIGSTOP sampling over 500 runs;
- **wall time**: `tools/bench.c`, min of 200 runs, one pinned core;
- **memory**: `/usr/bin/time -v` and `tools/liveset.py`, a model of the allocator.

## What executes

| kind | instructions | share |
|---|---|---|
| vector (zmm/ymm/xmm) | 2,766,562 | 61.5% |
| scalar bookkeeping in GPRs | 1,343,169 | 29.9% |
| mask-register ops | 279,417 | 6.2% |
| `vpgatherdd` (16 serial loads each) | 74,862 | 1.7% |
| `vpscatterdd` (16 serial stores each) | 32,039 | 0.7% |
| startup/exit stub | 82 | 0.002% |

The first working compiler was 31% vector / 61% bookkeeping.

## The islands

| # | island | measured cost | why it is still there |
|---|---|---|---|
| 1 | **Startup/exit stub** (`stencils/stub.S`): two mmaps, read loop, write loop, exit, fail handler. Hand-written, branchy, scalar. | 82 instructions, 6 syscalls per run | Syscalls. The language cannot express them and should not. |
| 2 | **Per-line bookkeeping**: loop counter and back-edge (4 instr/iteration), one pointer load per vector operand per iteration, gather/scatter bounds branches. | 29.9% of instructions | x86 has too few GPRs to keep a pointer per operand across the loop, so each load re-reads its slot. Length checks, allocation and carry init *were* here too and have been hoisted into per-token prologues. |
| 3 | **Allocator and release** (in stencils): size-class free list pop-or-bump with a branch, heap-limit check, release with a static-data branch. | once per storing token per line, ~15 instructions; included in row 2's 29.9% | Per line, not per element. Branches chosen over cmov for clarity; they are perfectly predicted. |
| 4 | **`halt`**: a conditional `exit_group` syscall in generated code, 13 times in the compiler. | 13 x ~8 instructions | Error exits. A compile error is decided by a vector reduction, but leaving is a branch. |
| 5 | **`vpgatherdd`**: 72 in the compiler source. | 74,862 executions ~ 1.2 M lane loads. HARDWARE.md: 1.14 ticks/lane, the same as a scalar load loop. | Token<->byte joins, table lookups, the stencil-byte gather. Branch-free and fused, but serial inside the CPU. This is the "SIMD" that isn't. |
| 6 | **`vpscatterdd`**: 37 in the source. | 32,039 executions ~ 0.5 M lane stores; 1.3 ticks/lane, *slower* than scalar stores. | The scatter race, per-line tables, operand patching. The algorithm only needs "last writer wins"; the instruction is serial microcode. |
| 7 | **Scan carries** (`scan`, `scanmax`, `prev`: 20 in the source). | At best 2x over scalar (0.42 vs 0.85 ticks/element). | A prefix sum has a real dependency between chunks: log-step in register plus one carry register. |
| 8 | **Scalar lines**: 32 of the compiler's 149 lines (plus 35 generated constants) compute one value, as a vector loop of length 1. | 67 one-lane loops per compile; small, not separately measured | Uniformity: there is no second code generator for scalars. |
| 9 | **Page faults** (kernel). | 816 minor faults; a fault costs ~1.45 us here (measured: writing 7.4 MB of fresh memory takes 2.9 ms), so roughly 1.2 ms of the 2.6 ms run. The scalar C compiler takes 563. | Whole-array semantics materialise intermediates. Reduced from 1,893 by liveness-based release, tee fusion, chunk-local vectors and in-loop compression. The rest is ~7 byte-sized i32 arrays, ~22 token-sized arrays and 3 code-sized arrays live at peak. |
| 10 | **Bytes are widened to i32.** | 4x the memory and 4x the iterations of every byte-level loop; byte-level phases are ~55% of sampled time. HARDWARE.md: byte classification with `vpermi2b` would be 40x cheaper per byte than what the dword path can do. | One element type keeps the language, the stencil table and three implementations small. This is the largest remaining cost and the first thing to change. |
| 11 | **Stencil bytes come from GNU `as`** (`tools/mkstencils.py`, at build time). Tables (class table, deltas, …) come from Python. | 0 at compile time; 6,320 stencil bytes and ~35 KB of generated data lines in the compiler's source | Strand has no assembler. The compiler *selects, lays out, copies and patches* machine code; it does not *encode* instructions. The x86 encoder is the largest piece of the toolchain not written in Strand. |
| 12 | **Name resolution is incomplete by design.** A name can lose all four rounds (E_RACE), or two names can share a 64-bit fingerprint (E_COLLISION), and the program is rejected. | Never observed since the redesign; 40,000 distinct names resolve. **Untested**: I cannot construct an input for either error. | A complete method needs a sort or an unbounded loop. Instead the race is specified (`boot/spec.py`) and all implementations agree on what is rejected. A wart in the language, not in one compiler. |
| 13 | **Resource limits**: heap 64 GiB, output 4 GiB, input < 2 GiB; exceeding them exits 98/99. | 3 instructions per allocation / output | Not differentially tested: the interpreter cannot model them. |
| 14 | **Stage 0 (Python) and the C baseline.** | not part of the fixed point | Bootstrap and reference only. |

## The uncomfortable total

Wall time, min of N runs. Process floor (a trivial Strand binary) is 0.22 ms.

| input | scalar C (`boot/strandc.c`, gcc -O2) | Strand/SIMD (stage2) | ratio |
|---|---|---|---|
| its own source, 44.7 KB | 1.83 ms | 2.61 ms | 1.43x slower |
| 346 KB (8 renamed copies) | 11.0 ms | 21.0 ms | 1.9x slower |
| 2.79 MB (64 renamed copies) | 127 ms | 200 ms | 1.57x slower |

**The data-parallel compiler is slower than the obvious scalar one at every size measured.**
It started 2.5-3.9x slower. The C compiler is not tuned either (it is the obvious
character loop with a hash table and `qsort`).

## History of this ledger

| change | self-compile | faults | vector share |
|---|---|---|---|
| first working compiler (30 KB source then) | 3.69 ms | 1,893 | 31% |
| + release at last occurrence, free lists | 3.02 ms | not recorded | |
| + vector lane mask, hoisted prologues (source grew) | 3.46 ms | 1,272 | 55% |
| + tee fusion, spec'd 4-round race, limits (source 42.7 KB) | 2.97 ms | 1,170 | 61% |
| + chunk-local vectors (64-byte slots) | 2.78 ms | 940 | |
| + in-loop compression of segmented sums; quote check folded in | 2.61 ms | 816 | 61.5% |
