# MORNING: campaign 1 (linking at hardware speed), as of 2026-11-13T18:21:56Z

## The number

MinIO, 8 cores, whole process by the caller's clock: stock `go tool link -s -w` 1,750 ms; fclink 303 ms;
**hlink 167-182 ms** (best interleaved minima today: 179 ext4 overwriting the old output, 173 tmpfs, 167-174
hugetlbfs). Hardware floor 9.7 ms (6.9 before random-access terms were added to the model): **about 5% of
floor**. kube-apiserver: 267 ms against 13.8 ms. The machine was shared with builders and test suites all
day; FLOOR.md's table (below) was taken that way and its 8-core MinIO row (243 ms) is a disturbed run.

| workload/phase | bytes in | bytes out | ms (1 core) | ms (2) | ms (4) | ms (8 cores) | floor ms (8 cores) | % of floor | binds |
| workload/phase | bytes in | bytes out | ms (1 core) | ms (2) | ms (4) | ms (8 cores) | floor ms (8 cores) | % of floor | binds |
| minio/resolve | 108.0 MB | 10.8 MB | 101.7 | 55.3 | 39.8 | 28.1 | 1.36 | 4.8% | memory |
| minio/reach | 77.7 MB | 0.4 MB | 144.0 | 87.6 | 63.3 | 114.8 | 2.78 | 2.4% | memory |
| minio/layout | 7.6 MB | 1.4 MB | 74.2 | 43.2 | 27.6 | 37.0 | 0.60 | 1.6% | memory |
| minio/write | 160.7 MB | 109.5 MB | 125.2 | 66.3 | 36.8 | 38.7 | 4.92 | 12.7% | memory |
| minio/OS: open inputs | | | 19.3 | 11.5 | 19.2 | 14.3 | 3.98 | 27.8% | kernel |
| minio/OS: create output | | | (in write) | (in write) | (in write) | (in write) | 27.52 |  | kernel |
| minio/OS: map inputs, exit | | | 4.9 | 4.5 | 10.3 | 4.0 | 10.31 | 257.6% | kernel |
| **minio/total (process, warm)** | 841.8 MB | 109.5 MB | 473.1 | 271.1 | 199.5 | **243.3** | **9.65** (hardware) | **4.0%** | |
| minio/total against hardware + OS limits | | | | | | 243.3 | 51.46 | 21.2% | |
| minio/total (process, cold) | | | | | | 359.3 | 93.61 (disk read of the 261 MB touched) | 26.1% | disk |

## Oracle and milestones

M0 floors + frozen objects + harness; M1 hello; M2 all 259 std packages with tests; **M3 MinIO's 46 test
packages** (`cmd`: 710 pass, 1 fails with both linkers). kube-apiserver's 18 test packages are running.
secdiff (symbolic section-by-section diff against the stock linker's binary) works and found 4 real
differences inherited from fclink (go:fipsinfo never written: fixed; which DUPOK copy is kept; alignment of
merged symbols; alignment of zero-size symbols): a builder is driving it to zero on hlink.

## What was built today

- `link/`: hlink (C, 4.5 k lines), freeze/bench/floor scripts, try/stdtest/secdiff oracles, prof.sh.
- Speed work that landed: partitioned 128-bit-key joins; DWARF symbols left out; content-addressed symbols
  joined after reachability (1.29 M of 2.68 M); staged, prefetching flood on compact flags; branch-free
  reference decoding; spin-then-futex pool; huge pages for the linker's arrays; overwrite-in-place,
  hugetlbfs output, detached teardown.

## Ideas tried and killed

- Pre-populating input page tables at load (MADV_POPULATE_READ): load 9-12 -> 35-63 ms, flood no faster.
- Parallel MADV_DONTNEED before exit: 95 ms of kernel CPU; the fork that returns early wins.
- Resolving by-name references lazily / per-package name indexes: replaced by skipping DWARF kinds (4 of 5
  by-name references), which is exact and free.

## What I think is true and uncomfortable

Single-core work is 480 ms (load 36, resolve 100, reach 136, layout 74, create 26, write 97). Each phase
has a known next step (PLAN.md, Now), and I expect 35-45 ms on 8 cores from this design: the kill line
(10% of floor) is plausible, the exit line (33%) is not, on stock objects with 4 KiB pages: a dependent
cache miss costs 158 ns here (nested paging), and the object format scatters every symbol's records.

## Decisions that are yours

See STATUS.md: scope (-s -w), C instead of Strand, and whether a cached per-archive sidecar index or
inputs on hugetlbfs are in bounds for the exit test.
