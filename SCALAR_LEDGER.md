# SCALAR_LEDGER.md

Every place where the "everything is data-parallel" claim is not true, what it costs, and
why it is still there. Numbers are for stage2 compiling its own source (37 KB, ~10 K tokens)
on the machine in HARDWARE.md, measured with `tools/prof.c` (ptrace single-step counts and
time sampling) and `tools/bench.c`. *This file is updated as islands are removed; see the
end for history.*

(Initial version — numbers are refreshed in the final section of REPORT.md.)

| # | island | measured cost | why it is still there |
|---|---|---|---|
| 1 | Startup/exit stub: mmap, read loop, write loop, exit, fail handler (`stencils/stub.S`) | 100 instructions per run; 5 syscalls | Syscalls are inherently scalar. The language has no way to express them and should not. |
| 2 | Per-line bookkeeping in GPRs: loop counter, back-edge, pointer load per operand, allocator, length checks | 36.9% of executed instructions (was 60.7% before hoisting) | One pointer load per vector operand per iteration because x86 has too few GPRs to hold a pointer per operand; the loop counter and branch are the loop itself. |
| 3 | `vpgatherdd` | 119,712 executions x 16 lanes. HARDWARE.md: 1.14 ticks/element = a scalar load loop | Used for token->byte and byte->token joins, table lookups, and the stencil-byte gather. Branch-free and fused, but not parallel inside the CPU. |
| 4 | `vpscatterdd` | 12,378 executions x 16 lanes; 1.3 ticks/element, slower than scalar stores | The scatter race and operand patching. The *algorithm* is order-independent except for the documented last-writer-wins rule; the *instruction* is serial microcode. |
| 5 | Scan carries (`scan`, `scanmax`, `prev`) | 2x over scalar at best (HARDWARE.md: 0.42 vs 0.85 ticks/element) | A prefix sum has a true dependency between chunks. In-register log-step plus one carry register. |
| 6 | Scalar lines (`N 1 - >M`) run as vector loops of length 1 | ~70 lines of the compiler; each costs a full line prologue (~60 instructions) for one useful lane | Uniformity: no second code generator for scalars. |
| 7 | Page faults (kernel) | 1,272 minor faults ~ 1.8 ms of a 3.5 ms compile | Fresh memory costs ~1.5 us/page in this VM. Reduced from 1,893 by liveness-based release; the remaining live set is ~5 MB because bytes are stored as i32. |
| 8 | Stencil bytes are produced by GNU `as` at build time (`tools/mkstencils.py`), tables by Python | offline; 0 at compile time | Strand has no assembler. The compiler copies and patches bytes it did not encode. The x86 encoder is the largest thing *not* written in Strand. |
| 9 | Stage 0 (Python) and the C baseline | not part of the fixed point | Bootstrap and reference only. |
| 10 | Bytes are widened to i32 | 4x memory and 4x iterations in every byte-level phase; byte classes + tokens = 52% of sampled time | One element type keeps the language and the stencil table small. |

## The uncomfortable total

| compiler | 37 KB (self) | 299 KB | 2.4 MB |
|---|---|---|---|
| scalar C (`boot/strandc.c`, gcc -O2) | 1.39 ms | 7.5 ms | 77 ms |
| Strand/SIMD (stage2) | 3.50 ms | 29.2 ms | 274 ms |

The data-parallel compiler is 2.5-3.9x **slower** than the obvious scalar one. Process
floor is 0.2 ms. The reasons, in measured order: page faults (island 7), ~250 separate
passes over arrays that do not fit L1 (island 10), gather/scatter (3, 4).
