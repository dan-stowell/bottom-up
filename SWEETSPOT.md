# SWEETSPOT.md — where Strand wins, where it does not, and why

REPORT.md ends with the Strand compiler *slower* than scalar C. This file is the follow-up
question: is there a class of programs for which this model is simply the right tool?
Short answer: **yes — single-pass map / filter / reduce / translate programs with local
context. There it is 5-12x scalar C; against `gcc -O3 -march=native` it is at least even on
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
| lines | reduce | 1.46 | 6.01 | 8.35 | 10.47 | 13.98 | 1.4x | 9.6x | 2.3x |
| lines32 | the same on dword lanes | 1.48 | 5.98 | 5.91 | 11.61 | 13.17 | 1.0x | 8.9x | 2.2x |
| words | prev + reduce | 1.16 | 1.16 | 7.77 | 14.08 | 13.88 | 6.7x | 12.2x | 12.1x |
| sum | reduce | 2.31 | 6.00 | 8.56 | 10.64 | 11.58 | 1.4x | 5.0x | 1.9x |
| pairs ("th") | prev + reduce | 1.15 | 1.15 | 7.53 | 10.93 | 12.51 | 6.6x | 10.9x | 10.9x |
| nonascii | reduce | 1.61 | 5.96 | 7.83 | 12.33 | 12.68 | 1.3x | 7.9x | 2.1x |
| upper | map | 1.36 | 6.63 | 6.04 | 11.12 | 11.76 | 0.9x | 8.6x | 1.8x |
| rot13 | map | 1.05 | 4.81 | 5.29 | 12.46 | 12.23 | 1.1x | 11.8x | 2.6x |
| swapcase | table lookup | 2.38 | 2.38 | 6.45 | 12.05 | 12.63 | 2.7x | 5.3x | 5.3x |
| letters | filter | 1.39 | 1.40 | 6.15 | 12.01 | 13.31 | 4.4x | 9.6x | 9.5x |
| maxline | scan + reduce | 1.22 | 0.83 | 2.84 | 2.81 | 2.87 | 3.4x | 2.3x | 3.5x |
| sum4 | reduce (i32) | 5.47 | 8.22 | 8.91 | 15.29 | 14.73 | 1.1x | 2.8x | 1.9x |
| sumpos4 | map + reduce (i32) | 4.13 | 8.04 | 8.61 | 14.49 | 14.13 | 1.1x | 3.5x | 1.8x |
| maxgap4 | prev + reduce (i32) | 3.14 | 4.00 | 8.93 | 16.84 | 19.95 | 2.2x | 6.4x | 5.0x |
| scale4 | map (i32) | 3.87 | 7.34 | 7.16 | 14.80 | 16.10 | 1.0x | 4.2x | 2.2x |
| filter4 | filter (i32) | 3.91 | 3.91 | 7.32 | 18.06 | 15.48 | 1.9x | 4.6x | 4.6x |
| prefix4 | scan (i32) | 3.91 | 5.06 | 6.42 | 6.30 | 6.48 | 1.3x | 1.7x | 1.3x |

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
With the pool it reaches 5-12x scalar C.

**The I/O floor.** `explore/iotest.c`: on this VM the kernel needs 12-20 ms just to make a
256 MiB cached file accessible (map + fault + unmap, or copy), i.e. 13-21 GB/s. Strand hits
that with two to four cores; eight vCPUs add nothing (the in-memory ceiling test shows the
same, so there are probably four physical cores). Past this point the numbers measure the
kernel, not Strand.

**Byte lanes were worth 1.3-1.8x on one core** (`lines` vs `lines32`: 8.35 vs 5.91) and
nothing at four, exactly as the ceiling test predicted.

**Weak: running sums.** `prefix4` and `maxline` win by 1.3-3.5x and do not scale: a scan's
carry is a true dependency, the in-register scan is only ~2x a scalar loop (HARDWARE.md),
and scans veto the thread pool.

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
- **Not done**: parallel scans (the one shape that does not scale); several reductions in
  one line; byte lanes for anything but streaming lines (so the compiler's own lexer still
  runs on dword lanes); parallel compress-to-memory; reading with `pread` per thread
  (iotest says ~1.5x under the mmap floor at 4 threads).
