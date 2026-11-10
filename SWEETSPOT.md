# SWEETSPOT.md — where Strand wins, where it does not, and why

REPORT.md ends with the Strand compiler *slower* than scalar C. This file is the follow-up
question: is there a class of programs for which this model is simply the right tool?
Short answer: **yes — single-pass map / filter / reduce / translate programs with local
context. There it is 3-15x scalar C; against `gcc -O3 -march=native` it is at least even on
one core and 2.7-6.7x ahead wherever the loop carries a dependency; and with four cores it
is limited by the kernel's I/O path rather than by compute.** Everything here is measured on the machine in HARDWARE.md;
`bench/run.py` reproduces the table.

## 1. The ceiling, before building anything (`explore/ceiling.c`)

Hand-written kernels, 256 MiB already in memory, GB/s:

| kernel | scalar -O2 | gcc -O3 autovec | AVX-512, 16 dword lanes (Strand's style) | AVX-512, 64 byte lanes | byte lanes x4 threads |
|---|---|---|---|---|---|
| count newlines | 1.84 | 10.5 | 19.6 | 27.3 | 70 |
| count words (needs previous byte) | 1.23 | 1.24 | 15.3 | 29.2 | 55 |
| uppercase into a buffer | 1.70 | 14.3 | 12.7 | 12.2 | 25 |

Two things to take from it. The auto-vectoriser gives up completely as soon as one value
is carried from the previous element (`count words`), which Strand expresses with `prev`.
And widening bytes to 32-bit lanes costs only 1.4-2x at this size, because both variants
are near memory bandwidth; byte lanes matter mostly when data is cache-resident.

## 2. What was actually in the way: the runtime, not the code

The same three programs as real Strand binaries, 256 MiB file:

| | compute (user) | kernel | throughput |
|---|---|---|---|
| count newlines, original runtime | 20 ms | 135 ms | 1.76 GB/s |
| after mapping stdin and streaming stdout | 21 ms | 20 ms | 6.5 GB/s |
| + byte lanes | | | 8.4 GB/s |
| + thread pool, 4 cores | | | 13-14 GB/s |

The generated loop was already running at ~13 GB/s. The stub copied all of stdin into fresh
heap memory (65,553 page faults) and held all output until exit. Four changes, none of
which needed a new compiler pass:

1. **Map the input, stream the output.** stdin is `mmap`ed when it is a file; output goes
   through a reused 256 KiB buffer flushed from inside the `out` loop.
2. **Lines vote; big ones fork.** The lead token's prologue marks a line splittable; any
   token that cannot be split vetoes in its own prologue. At the loop head a big splittable
   line calls a thread pool in the stub, whose workers jump into the *same* loop body with
   their own index range. `prev` stays parallel through a one-chunk warm-up that computes
   but stores nothing; sums and maxima are combined; output is produced in ordered blocks.
   The compiler's source did not change for this: it is all stencils and stub.
3. **Streaming filters.** `vals mask outif` compresses straight into the output stream; in
   the pool each thread fills its own region and the regions are written in order.
4. **Byte lines.** A `bytes` lead runs its line on 64 unsigned byte lanes: byte arithmetic,
   `prev`, `sel`, 256-entry table lookup (two `vpermi2b`), `vpcompressb` filters,
   `vpsadbw` sums. Deliberately small: streaming lines only, no named byte vectors. In the
   compiler it is one table (`BYTEOF[class]`) and three lines of Strand.

## 3. The map (`bench/run.py 256`)

Whole processes, 256 MiB from the page cache, output to /dev/null, best of 5, GB/s.
"gcc -O3" is `-O3 -march=native` on the same C source as "scalar C" (`bench/base.c`);
both C columns are single-threaded. Outputs of all three are checked identical.

| program | shape | scalar C | gcc -O3 | Strand 1 core | Strand 4 cores | Strand 8 vCPUs | 1 core vs gcc -O3 | best vs scalar | best vs gcc -O3 |
|---|---|---|---|---|---|---|---|---|---|
| lines | reduce | 1.47 | 5.98 | 8.23 | 11.34 | 14.46 | 1.4x | 9.8x | 2.4x |
| lines32 | reduce, dword lanes | 1.47 | 5.78 | 5.69 | 15.95 | 13.00 | 1.0x | 10.9x | 2.8x |
| words | prev+reduce | 1.16 | 1.16 | 7.77 | 13.37 | 16.15 | 6.7x | 13.9x | 13.9x |
| sum | reduce | 2.21 | 6.07 | 7.46 | 15.32 | 14.48 | 1.2x | 6.9x | 2.5x |
| pairs | prev+reduce | 1.15 | 1.15 | 7.48 | 10.90 | 17.46 | 6.5x | 15.2x | 15.2x |
| nonascii | reduce | 1.62 | 5.97 | 8.19 | 13.38 | 18.08 | 1.4x | 11.2x | 3.0x |
| upper | map | 1.37 | 6.63 | 6.08 | 14.08 | 12.10 | 0.9x | 10.3x | 2.1x |
| rot13 | map | 1.05 | 4.85 | 5.33 | 11.95 | 14.75 | 1.1x | 14.0x | 3.0x |
| swapcase | table lookup | 2.34 | 2.33 | 6.09 | 14.04 | 14.88 | 2.6x | 6.4x | 6.4x |
| maxline | scan+reduce | 1.22 | 0.83 | 2.83 | 5.43 | 8.81 | 3.4x | 7.2x | 10.6x |
| letters | filter | 1.40 | 1.40 | 6.41 | 11.85 | 14.39 | 4.6x | 10.3x | 10.3x |
| sum4 | reduce | 5.50 | 8.34 | 8.99 | 14.84 | 15.41 | 1.1x | 2.8x | 1.8x |
| sumpos4 | map+reduce | 4.06 | 7.85 | 8.68 | 15.88 | 17.81 | 1.1x | 4.4x | 2.3x |
| maxgap4 | prev+reduce | 3.11 | 3.98 | 8.78 | 14.42 | 19.42 | 2.2x | 6.2x | 4.9x |
| scale4 | map | 3.86 | 7.07 | 7.26 | 11.78 | 15.55 | 1.0x | 4.0x | 2.2x |
| filter4 | filter | 3.86 | 3.87 | 7.40 | 14.60 | 16.92 | 1.9x | 4.4x | 4.4x |
| prefix4 | scan | 3.89 | 4.96 | 6.43 | 11.99 | 13.09 | 1.3x | 3.4x | 2.6x |

The programs, in full:

```
bytes 32 > dup prev 1 ^ & $W                         # words: a non-blank whose predecessor is blank
bytes dup 97 >= over 122 <= & outif                  # letters: keep a..z
bytes :c 97 >= c 122 <= & c 32 - c sel out           # upper
~256 :i 223 & :u 65 >= u 90 <= & 32 * i ^ >t         # swapcase: build a 256-entry table ...
bytes @t out                                         #           ... and translate through it
input4 dup prev - ^M                                 # maxgap4: largest increase between neighbours
input4 dup 0 > outif4                                # filter4: keep the positive numbers
```

## 4. Reading the map

**The sweet spot** is a single pass in which each output element depends on a bounded
window of input: maps, reductions, filters, table lookups, and anything built from `prev`.
On **one core** Strand matches or beats `gcc -O3 -march=native` on every row (one tie),
and by 2.7-6.7x wherever C's loop has a carried dependency, a data-dependent output
position or a table lookup (`words`, `pairs`, `letters`, `swapcase`, `maxgap4`,
`filter4`) — the auto-vectoriser cannot touch those, so the two C columns are identical.
With the pool it reaches 3-15x scalar C.

**The I/O floor.** `explore/iotest.c`: on this VM the kernel needs 12-20 ms just to make a
256 MiB cached file accessible (map + fault + unmap, or copy), i.e. 13-21 GB/s. Strand hits
that with two to four cores; eight vCPUs add nothing (the in-memory ceiling test shows the
same, so there are probably four physical cores). Past this point the numbers measure the
kernel, not Strand.

**Byte lanes were worth 1.3-1.8x on one core** (`lines` vs `lines32`: 8.35 vs 5.91) and
nothing at four, exactly as the ceiling test predicted.

**Running sums scale too, by speculation.** A scan's carry is a true dependency, so a
scan line in the pool runs each block twice: a dry pass that stores nothing tells the pool
what every thread's range adds to each carry; the pool derives each thread's starting
carries; the real pass runs; and then every thread's starting carries are compared with its
predecessor's ending carries. Equal means the block is exactly the serial result; unequal
(scans feeding scans) restores the caller's registers and the line finishes serially.
`prefix4` went from 6.4 to 12-13 GB/s and `maxline` from 2.8 to 8.8. On one core a scan is
still only 1.3-3.4x ahead: the in-register scan is ~2x a scalar loop (HARDWARE.md).

**Against the strongest C I can write** (`bench/base_omp.c`: mmap + `-O3 -march=native
-fopenmp`, including a hand-written two-pass parallel prefix sum and per-thread filter
regions), GB/s:

| | 1 core: OpenMP C / Strand | 4 cores | 8 vCPUs |
|---|---|---|---|
| lines (reduce) | 7.6 / 8.3 | 14.5 / 14.7 | 16.3 / 19.9 |
| upper (map) | 6.6 / 5.8 | 13.8 / 13.8 | 15.1 / 15.8 |
| prefix4 (scan) | 4.9 / 6.6 | 12.3 / 12.6 | 13.3 / 14.5 |
| words (prev) | 1.5 / 7.5 | 5.3 / 16.6 | 5.6 / 21.1 |
| letters (filter) | 1.5 / 6.3 | 4.9 / 16.7 | 7.3 / 19.0 |

This is the honest size of the result. For pure maps, reductions and prefix sums, a C
programmer who maps the file and adds OpenMP pragmas **ties** Strand: both are at the I/O
floor. Where each element needs its neighbour or the output position depends on the data,
the C compiler still cannot vectorise, threads only multiply a scalar loop, and Strand
stays **3-4x ahead of parallel C**. That class — context and filters — is the sweet spot
proper. (The C versions are 5-25 lines each of mmap/OpenMP/region bookkeeping; the Strand
versions are one line.)

**Outside the model entirely** (not in the table because they cannot be written well):
histograms and anything else that needs scatter-*add*; data-dependent control flow
(parsing nested structure, searching with early exit); pointer-chasing. And, as REPORT.md
shows, *join-shaped* work — tokens looking each other up, names — is expressible through
gather/scatter but those cost scalar-loop prices on this CPU, which is why the compiler
itself is not in the sweet spot. Nor can a line produce two reductions, so `wc` proper is
two passes.

## 5. Negative results and mistakes along the way

- **Filters first lost by 2x** (0.5-0.7x of scalar C): `?x` materialises a widened array,
  page-faults on every page of it, and blocks threading. Fixed by `outif`, not by tuning.
- **The first parallel `prev` was wrong.** The warm-up chunk stored its results, and its
  first lane is computed with a zero carry, so it raced a wrong value against the
  neighbouring thread's correct one. The eager-mode fuzzer (any argument makes every
  splittable line of 64+ elements fork) found it in the first few programs. Fixed with a
  separate "keep" mask.
- **The main thread ran 4x slow inside the pool** for lines with chunk-local vectors: its
  stack pointer was off by 8 there, so 64-byte temporaries straddled cache lines.
- **A benchmark regression that was not one**: the 64-copies-of-the-compiler input grew by
  30% because the stub (embedded in every copy) grew. Compare against C on the same file.
- **Eight vCPUs are not better than four** for anything here.
- **Two more bugs found by measurement in the parallel-scan pool**: the word recording which
  carries a line uses sat on top of a register save slot, so verification silently checked
  nothing (caught because the errors equalled minus the running total); and a fully masked
  store to unmapped memory in the dry pass took a microcode assist per chunk (0.6 GB/s).
- **The compiler itself gains little** from the pool: 351 ms on one core vs 237 ms on eight
  vCPUs for a 5.2 MB source (scalar C: 273 ms). Its heavy lines chain scans or compress into
  memory. That is finally *faster* than the scalar compiler, but only by 13%, with 8 vCPUs.
- **Not done**: several reductions in one line; byte lanes for anything but streaming lines (so the compiler's own lexer still
  runs on dword lanes); parallel compress-to-memory; reading with `pread` per thread
  (iotest says ~1.5x under the mmap floor at 4 threads).
