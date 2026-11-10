# SWEETSPOT.md — where Strand wins, where it does not, and why

REPORT.md ends with the Strand compiler *slower* than scalar C. This file is the follow-up
question: is there a class of programs for which this model is simply the right tool?
Short answer: **yes — single-pass map / filter / reduce programs with local context — and
there it is 6-10x scalar C and 2-10x `gcc -O3 -march=native`, limited by the kernel's I/O
path rather than by compute.** Everything here is measured on the machine in HARDWARE.md;
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
| + thread pool, 4 cores | | | 14 GB/s |

The generated loop was already running at ~13 GB/s. The stub copied all of stdin into fresh
heap memory (65,553 page faults) and held all output until exit. Three runtime changes,
none of which touched the compiler's source:

1. **Map the input, stream the output.** stdin is `mmap`ed when it is a file; output goes
   through a reused 256 KiB buffer flushed from inside the `out` loop.
2. **Lines vote; big ones fork.** The lead token's prologue marks a line splittable; any
   token that cannot be split vetoes in its own prologue. At the loop head a big splittable
   line calls a thread pool in the stub, whose workers jump into the *same* loop body with
   their own index range. `prev` stays parallel through a one-chunk warm-up that computes
   but stores nothing; sums and maxima are combined; output is produced in ordered blocks.
3. **Streaming filters.** `vals mask outif` compresses straight into the output stream; in
   the pool each thread fills its own region and the regions are written in order.

## 3. The map (`bench/run.py 256`)

Whole processes, 256 MiB from the page cache, output to /dev/null, best of 5, GB/s.
"gcc -O3" is `-O3 -march=native` on the same C source as "scalar C" (`bench/base.c`).

| program | shape | scalar C | gcc -O3 | Strand 1 core | Strand 4 cores | Strand 8 vCPUs | best vs scalar | best vs gcc -O3 |
|---|---|---|---|---|---|---|---|---|
| lines | reduce | 1.47 | 5.86 | 5.75 | 12.99 | 13.20 | 9.0x | 2.3x |
| words | prev + reduce | 1.17 | 1.16 | 5.03 | 9.61 | 10.48 | 9.0x | 9.0x |
| sum | reduce | 2.22 | 6.09 | 7.02 | 12.33 | 13.14 | 5.9x | 2.2x |
| pairs ("th") | prev + reduce | 1.14 | 1.14 | 4.52 | 9.49 | 9.96 | 8.7x | 8.7x |
| nonascii | reduce | 1.56 | 5.85 | 5.64 | 11.29 | 11.32 | 7.3x | 1.9x |
| upper | map | 1.37 | 6.25 | 3.91 | 8.83 | 13.00 | 9.5x | 2.1x |
| rot13 | map | 1.05 | 4.57 | 2.93 | 8.34 | 9.70 | 9.2x | 2.1x |
| letters | filter | 1.40 | 1.39 | 3.39 | 10.29 | 10.18 | 7.3x | 7.4x |
| maxline | scan + reduce | 1.22 | 0.83 | 2.82 | 2.80 | 2.77 | 2.3x | 3.4x |
| sum4 | reduce (i32) | 5.53 | 8.33 | 8.96 | 14.88 | 19.59 | 3.5x | 2.4x |
| sumpos4 | map + reduce (i32) | 4.15 | 8.29 | 8.71 | 14.60 | 17.00 | 4.1x | 2.1x |
| maxgap4 | prev + reduce (i32) | 3.12 | 4.01 | 8.95 | 16.45 | 19.27 | 6.2x | 4.8x |
| scale4 | map (i32) | 3.84 | 7.25 | 7.03 | 12.44 | 13.55 | 3.5x | 1.9x |
| filter4 | filter (i32) | 3.89 | 3.90 | 7.40 | 12.15 | 13.80 | 3.6x | 3.5x |
| prefix4 | scan (i32) | 3.86 | 5.01 | 6.56 | 6.41 | 6.01 | 1.7x | 1.3x |

Each Strand program is one or two lines, e.g. `input 32 > dup prev 1 ^ & $W` (count words)
and `input dup 97 >= over 122 <= & outif` (keep lowercase letters).

## 4. Reading the map

**The sweet spot** is a single pass in which each output element depends on a bounded
window of input: maps, reductions, filters, and anything built from `prev`. There Strand is
on one core already 3-5x scalar C on bytes, and it scales to the machine. The largest
margins over an optimising C compiler are exactly where C's loop has a carried dependency
or a data-dependent output position (`words`, `pairs`, `letters`, `filter4`, `maxgap4`):
gcc cannot vectorise those at all, so the scalar and -O3 columns are identical.

**The I/O floor.** `explore/iotest.c`: on this VM the kernel needs 12-20 ms just to make a
256 MiB cached file accessible (map + fault + unmap, or copy), i.e. 13-21 GB/s. Four cores
of Strand sit on that floor; eight vCPUs add nothing (the in-memory ceiling test shows the
same, so there are probably four physical cores).

**Weak: running sums.** `prefix4` and `maxline` win by 1.3-3.4x and do not scale: a scan's
carry is a true dependency, the in-register scan is only ~2x a scalar loop (HARDWARE.md),
and scans veto the thread pool.

**Outside the model entirely** (not in the table because they cannot be written well):
histograms and anything else that needs scatter-*add*; data-dependent control flow
(parsing nested structure, searching with early exit); pointer-chasing. And, as REPORT.md
shows, *join-shaped* work — tokens looking each other up, names — is expressible through
gather/scatter but those cost scalar-loop prices on this CPU, which is why the compiler
itself is not in the sweet spot.

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
- **Not done**: byte lanes (ceiling says up to 2x on one core, little at the I/O floor);
  parallel scans; parallel compress-to-memory; reading with `pread` per thread (iotest
  says ~1.5x under the mmap floor at 4 threads).
