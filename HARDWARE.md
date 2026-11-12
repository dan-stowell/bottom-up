# HARDWARE.md — what this machine actually is

Measured on the exe.dev VM this project was built on, 2026-11-10, with `hw/probe.c`
(`gcc -O2 -o probe probe.c && taskset -c 2 ./probe`; raw output in `hw/probe.out.txt`).

## Identity

| | |
|---|---|
| CPU | AMD EPYC 9554P (Zen 4 "Genoa"), family 25 model 17, under KVM |
| vCPUs | 8 (1 thread/core as presented), 31 GiB RAM, no swap |
| Caches (as reported) | L1d 32 KiB/core, L2 1 MiB/core, L3 32 MiB |
| TSC | 3095 MHz, invariant. "Ticks" below are TSC ticks, **not** core cycles: the core may boost above 3.1 GHz, so 1 tick <= 1 cycle is not guaranteed. ns are wall-clock. |
| Toolchain | gcc 13.3, binutils (as/ld/objdump), Python 3.12, Go. No clang, nasm, rustc, perf, gdb. |

## Which extensions really execute

CPUID is advisory inside a VM, so each extension was tested by *executing* one of its
instructions under a SIGILL handler. Inputs go through a `volatile` so the compiler
cannot constant-fold the instruction away, and the disassembly was checked.

(The first version of the probe got this wrong: gcc folded `set1(3)+add` to a constant,
so the "probe" for AVX-512FP16 executed no FP16 instruction and reported "executes".
That would have been a false "the VM lies" finding. Lesson recorded.)

Result: **CPUID is honest here.** Everything advertised executes; nothing unadvertised does.

- Executes: SSE2..SSE4.2, SSE4a, POPCNT, LZCNT, BMI1, BMI2, ADX, MOVBE, PCLMULQDQ, AES, SHA,
  AVX, AVX2, FMA3, F16C, **AVX-512 F, DQ, BW, VL, CD, IFMA, VBMI, VBMI2, VNNI, BITALG,
  VPOPCNTDQ, BF16**, GFNI, VAES, VPCLMULQDQ (all at zmm width).
- SIGILL (and not advertised): AVX-512FP16, AVX-512VP2INTERSECT, AVX-VNNI (VEX), AMX, APX.
- XCR0 = 0xe7: opmask, ZMM_Hi256 and Hi16_ZMM state are all enabled by the guest kernel.

## What things cost

L1-resident (three 4 KiB arrays), best of 7, one pinned core.

| operation | ticks | per | note |
|---|---|---|---|
| add / mullo / min / varshift i32, zmm (load,load,op,store) | 1.9 | 16 elts | 0.12/elt |
| same, ymm | 0.9 | 8 elts | **zmm = 2 x ymm**: 512-bit is double-pumped |
| add i32, scalar | 1.1 | 1 elt | SIMD wins ~9x here |
| compare->k + blend, vpternlogd | 2.8 | 16 elts | |
| test->k, kmov->gpr, popcnt | 1.9 | 16 elts | mask<->GPR is cheap |
| `vpcompressd zmm{k}{z}` + full-width store | 4.4 | 16 elts | 0.27/elt; scalar branchless 0.86, branchy 2.4 |
| **`vpcompressd [mem]{k}`** | **71** | 16 elts | **16x slower than reg form. Never use.** |
| `vpcompressb zmm{k}{z}` + store | 4.9 | 64 bytes | |
| **`vpcompressb [mem]{k}`** | **144** | 64 bytes | same trap |
| `vpexpandd` | 4.9 | 16 elts | |
| `vpgatherdd` zmm, L1 table | 18.3 | 16 elts | 1.14/elt — **same as a scalar load loop (1.16)** |
| `vpgatherdd` zmm, 1,024 fixed indices spread over 16 MiB | 138 | 16 elts | a gather across many pages and cache lines; *not* a memory-bound walk of 64 MiB (see corrections) |
| `vpscatterdd` zmm, L1 | 20.8 | 16 elts | 1.3/elt — **slower than scalar stores (1.08)** |
| `vpconflictd` zmm | 1.8 | 16 elts | cheap on Zen 4 (it is ~20+ on Intel) |
| prefix sum, in-register log-step + carry | 6.7 | 16 elts | 0.42/elt vs scalar 0.85 (`-march=native`) or 1.7 (plain `-O2`): **2-4x**, depending on how the scalar loop is compiled |
| prefix-xor via pclmulqdq | 2.3 | 64 bits | |
| pext r64 | 0.87 | op | fast on Zen 4 (was microcoded on Zen 1/2) |
| vpermb / vpshufb zmm | 1.7 | 64 bytes | |
| byte classify, `vpermi2b` 128-entry table | 1.8 | 64 bytes | 0.03/byte |
| byte classify, 2 x vpshufb nibble tables | 2.6 | 64 bytes | |
| byte classify as dwords: widen + gather table | 17.6 | 16 bytes | 1.1/byte; a different kernel from the row above (dword output), about 40x its cost per input byte |
| vpmovzxbd widen / vpmovdb narrow | 1.7 / 1.0 | 16 elts | |
| 64 MiB streaming add, zmm vs ymm | 5.9 vs 5.8 | 64 bytes | identical: memory-bound |

Frequency: a scalar spin loop took the same time (ratio 0.998) immediately after 300 ms of
continuous zmm multiplies. One before/after comparison on this VM: no frequency penalty was
observed, which is weaker than "there is none".

## Corrections after the audit (2026-11-10)

AUDIT.md, defect 3, found that parts of this file claimed more than `hw/probe.c` measured.

- **Four "executes" probes did not contain their instruction.** The compiler had turned the
  SSE2, AVX and AVX2 intrinsics into other code and the ADX probe into a plain `adc`. They
  are now written in assembly, `hw/check_probe.py` checks every probe against its
  disassembly, and the four still execute. (The audit found two; the check found four.)
- **The scalar prefix-sum baseline overflowed a signed integer** (undefined behaviour). It
  now uses unsigned arithmetic. The scalar figure depends on the build: 0.85 ticks/element
  with `-march=native`, 1.69 with the documented plain `-O2` (five runs each, stable to
  0.02); the SIMD kernel is 6.7 per 16 elements either way. So the ratio is 2x or 4x, not
  "2x" as this file said; the second audit caught that my own correction contradicted the
  table. The table now gives the range.
- **"64 MiB table, random" is mislabelled.** That gather uses the same 1,024 indices every
  repetition, so it touches at most 1,024 cache lines below 16 MiB. It measures a gather
  across many pages, not a memory-bound traversal of 64 MiB.
- **"40x worse than vpermi2b"** compares two different kernels (a 128-entry byte table
  against a widened dword gather with four times the output). It is the cost ratio of those
  two representations, not of one task done two ways.
- **Costs are for load + operation + store kernels**, not instruction latencies. "Gather
  costs what a scalar load loop costs" is true of the kernel totals and does not show that
  gather lanes are serialised inside the CPU.
- **"No AVX-512 frequency penalty"** rests on one before/after comparison of a scalar loop.
  It is an observation about that run on this VM.
- The audit's rerun reproduced most rows closely; two differed: `vpcompressb` to a register
  8.7 (4.9 here) and `vpscatterdd` 27.6 (20.8 here).

## What this means for the design

1. **Zen 4's AVX-512 is a better instruction set, not a wider datapath.** zmm costs exactly
   two ymm ops. The win is masks, compress/expand, vpermb, conflict detection, and halved
   loop overhead — not 2x lanes. Design for the *instructions*.
2. **Gather and scatter kernels are no faster than scalar loops here.** A load + gather +
   store kernel costs what the scalar loop costs, a scatter kernel more. That is a
   statement about kernel totals; it does not show how the CPU executes the lanes. They buy
   *branch-freedom and fusion*, not speed, and every gather/scatter in the compiler is
   counted in SCALAR_LEDGER.md for that reason. Classify bytes with compares or byte
   permutes, never with a gathered table.
3. **Compress to a register, store full width.** Output arrays therefore need >= 64 bytes
   of slack after the logical end.
4. **Prefix sum is 2-4x over scalar** (the carry is a real dependency), far from the 9x of a
   plain add. Scans are the idiom, but they are not free; prefer designs where one scan
   serves many purposes.
5. Arrays that spill L1 (32 KiB = 8192 dwords) triple the cost of everything (5.2 vs 1.9
   ticks for add when three 16 KiB arrays were used). A compiler whose working set is
   "one dword per source byte" leaves L1 at 8 KiB of source. Byte lanes would be 4x denser.

## Limits for the build trace (2026-11-11)

`hw/limits.sh` (raw output: `hw/limits.out.txt`) measures what the floors in TRACE.md divide by.

| | |
|---|---|
| memcpy, 256 MiB blocks, 1 thread | 9.9 GB/s |
| memcpy, 8 threads together | 43.1 GB/s |
| disk, sequential read, cold (2 GiB, `dd`) | 3.1 GB/s |
| disk, sequential write with `fdatasync` | 0.90 GB/s |
| page cache to user space, 1 thread | 5.0 GB/s |
| MinIO's 7,640 source files (81 MB), cold, 8 readers | 0.27 GB/s, 25,500 files/s: **300 ms** |
| the same, page cache warm | 2.1 GB/s, 200,000 files/s: 38 ms |

Reading a build's sources as thousands of small files costs twelve times what the same bytes cost as
one sequential read (300 ms against 26 ms). That is 15% of a 2-second budget spent on directory
entries and inodes, before a byte is lexed.

## Page faults (2026-11-12, measured during a MinIO build)

- A minor fault on fresh anonymous memory costs about 3.6 us here (KVM guest, memory cgroup on): one
  compile of `minio/cmd` with the collector off takes 413 k faults and 1.45 s of kernel time. Zeroing a
  4 kB page at the measured memcpy rate would be 0.25-0.4 us.
- `madvise(MADV_POPULATE_WRITE)` on 4 MB at a time halves that (0.75 s).
- No transparent huge pages (`CONFIG_TRANSPARENT_HUGEPAGE` is not set), but hugetlbfs is there:
  `sysctl vm.nr_hugepages=N` reserves 2 MB pages, `mmap(MAP_HUGETLB)` uses them. Same compile: 12 k
  faults, 0.29 s kernel, and 0.7 s less user time (TLB). Hugetlb pages are not charged to a cgroup's
  MemoryMax (there is a separate hugetlb controller).
- Starting the 37 MB compiler binary and compiling nothing: about 9 ms of CPU, 700 faults.
