# JOURNAL (append-only: one line per commit, a paragraph at each milestone)

- 2026-11-13T17:23:30Z Campaign 1 started. Decisions, with why:
  - **The linker is written in C (`link/hlink.c`), not in Strand.** The floor is 13 ms for MinIO (below); a link
    is file mapping, a hash join, a graph flood, prefix sums and a relocating copy over 5.2 M symbol slots with
    irregular records (21/23/9-byte packed structs, varints, strings). Strand today is a loop-free language over
    dword lanes with no structs, no threads beyond its block pool and no mmap; growing it to parse goobj would
    be the campaign, not the link. Kernels that turn out SIMD-shaped (the relocating copy, name hashing) can
    move to Strand later where a measurement says they pay (CHARTER: "or leave it where a measurement says to").
  - **Start from simd-lang's `fclink` design** (flat arrays indexed by dense slot ids, partitioned joins,
    frontier flood, per-unit sizes + prefix sums, one write pass). On this VM today, frozen MinIO, 8 cores:
    stock `go tool link -s -w` 1,750-1,860 ms; fclink 295 ms whole process (233 ms by its own clock). It is the
    baseline to beat and the reference for semantics; an Opus builder is porting it to C in a worktree.
  - **Scope: `-s -w`** (no DWARF, no ELF symtab), statically linked linux/amd64, CGO off, as fclink. DWARF
    is 0.25 GB of the 0.84 GB of objects and none of the runtime needs it. (In STATUS.md for Dan.)
  - **Builds and measurements run on this VM**, not RBE: the repo has no Bazel workspace and the charter's
    number is "ms on this 8-vCPU machine against its measured limits".
  - **Oracle = three layers**: run the program and compare with the stock link (`link/try.sh`), the standard
    library's and the target's own tests through `-toolexec` (`link/stdtest.sh`), and a symbolic section
    diff against the stock linker's output (`link/secdiff`, a second builder): layout differs, so each
    symbol's bytes are compared modulo relocations, whose stock values are predicted from our map.
- 2026-11-13T17:23:30Z M0 pieces: `link/freeze.sh` (frozen MinIO: 1,311 archives + main.a, 949 MB, 842 MB of Go
  object, 5,205,427 symbol slots, 9.4 M relocations); `hw/linklimits.c` (limits: DRAM copy 43.4 GB/s on 8
  cores, 128 G instr/s, new-file pages 0.79 M/s on ext4 and 1.84 M/s on tmpfs, anon first-touch 4.3 M pages/s,
  munmap 9.9 M pages/s on one thread); `link/objstat.py`, `link/bench.sh`, `link/floor.py` -> FLOOR.md.
  **MinIO's hardware floor is 13.2 ms on 8 cores** (resolve 4.8, reach 1.5, layout 0.7, write 6.2); fclink is at
  4.5% of it. The OS limits as first measured add 48 ms (open 1,312 files 4 ms, create a 110 MB file 34 ms,
  fault in and unmap inputs 10 ms): **the kernel, not the memory bus, binds this link**, so the exit test
  (39 ms) needs the file-creation and teardown costs reshaped, not only the phases.
- 2026-11-13T17:30:31Z Codex review of the floor (REVIEWS.md): the floor was lenient by 2x. A copy at 43-52 GB/s moves each
  byte twice; measured separately, DRAM reads stream at 92.5 GB/s and non-temporal writes at 56.6 GB/s on 8
  cores. **MinIO's hardware floor is 6.9 ms, kube-apiserver's 11.7 ms**; fclink is at 2.3% / 2.2%. Kill line
  (10% of floor) = 69 ms for MinIO; exit line (3x) = 21 ms. kube-apiserver frozen (2,080 files, 1.97 GB of Go
  object, 13.2 M slots; fclink 532 ms). `explore/linkos/outfile.c`: 110 MB into a new ext4 file through a
  shared mapping costs 31-39 ms on 8 threads, into an existing cached file 13 ms, into fresh anonymous memory
  10.7 ms: it is the 4 KiB page fault (about 3 us each here) that costs, not the file system alone.
- 2026-11-13T17:31:49Z Spike: a Go binary execs from hugetlbfs if its segments are 2 MiB-aligned with filesz = memsz, and filling
  110 MB of a new hugetlbfs file takes 3.7 ms on 8 threads (ext4: 31-39 ms + 6 ms unmap). That removes the
  largest OS row from the link. IDEAS.md started (7 entries with kill tests).
- 2026-11-13T17:39:01Z Merged the builder's hlink (C port of fclink, link/*.c, 3,400 lines + generated gotables.h). Re-ran
  `link/test.sh` in the main checkout: ALL PASS (5 programs with and without inlining, -j 1/2/4/8 identical
  output for ifaces and MinIO, 5 std packages' tests agree with the stock link, MinIO --version, live set equal
  to fclink's 360,376). The builder found two fclink bugs (both only with -nodce: pc tables given data
  addresses; function table unsorted when a function is aligned above 32). **M1 reached.**
- 2026-11-13T17:47:55Z **M2.** `link/stdtest.sh` over every package of `go list std`: 254 agreed with the stock link, 5 differed
  (crypto/cipher, crypto/fips140, crypto/internal/fips140only, crypto/internal/fips140test, crypto/tls), 100
  have no tests, 2 need cgo. The 5 were one bug inherited from fclink: `go:fipsinfo` is *defined* by the check
  package (a linknamed variable) and the linker must overwrite it; fclink (and the port) only wrote it when it
  was undefined, so any binary run with GODEBUG=fips140=on died with "no verification checksum found". Fixed in
  link/special.c; the 5 pass; crypto/fips140 added to link/test.sh. Oracle number: 259 of 259 std test
  binaries; MinIO links and runs `--version` (its own tests are M3). hlink untuned: 188 ms for MinIO on 8
  cores. *What did the oracle number do since the last milestone?* 0 -> 5 programs -> 259 std test binaries in
  one session; nothing stalled. Next blocker: MinIO's own tests need a toolexec run over its packages, and
  the linker is at 3.7% of floor: the front half (resolve, reachability) is being rewritten around streaming
  pre-passes and partitioned joins (explore/linkos/lazy.c is the spike), the back half gets a hugetlbfs
  output mode (IDEAS.md #1).
- 2026-11-13T17:47:55Z Spike result (explore/linkos/lazy.c, MinIO, live set within 1 of fclink's before the fix below):
  resolving content-addressed symbols only when reached still reaches 1.29 M of the 2.68 M (every live
  function's pc tables and funcdata), so it halves that join rather than removing it, and costs byte-identity
  with the port; a by-name join over definitions + references is 1.4 M records of which 833 k references
  name only 45 k distinct symbols, almost all never defined (DWARF and file symbols): references must not be
  joined eagerly. Also: a pool that spins between jobs collapses when the machine is busy (flood 145 ms on
  1 thread, 321-507 ms on 8 with a test suite running beside it): spin briefly, then futex.
- 2026-11-13T18:08:12Z hlink front half, output byte-identical throughout (cmp against the port's MinIO binary after every step):
  resolve as 128-bit-key partitioned joins with DWARF-kind symbols left out (by-name records 1.4 M -> 0.63 M:
  four of five references by name are to debug information); a spin-then-futex pool; the flood on two
  interleaved byte arrays (flags the flood writes, facts it reads) instead of the 21 MB state words; leaves
  (no relocations, not a function) marked without being queued (678 k -> 220 k symbols flooded); references
  decoded branch-free (one table read; the switch was the most mispredicted branch in the program) and once
  per batch; batches of 64 symbols walked in stages so their misses overlap; object pages pre-populated at
  load (MADV_POPULATE_READ), because a prefetch of a page that is not in the page table yet is dropped.
  Measured on one core (the machine is shared with three builders, so 8-core walls are noise today):
  deadcode 158 -> 110 ms, resolve 207 -> 152 ms. What the flood costs now, core-ms: decode references 22,
  duplicates' canonical ids 15, process 41, the rest outside the batches. A dependent miss on this VM is
  158 ns (4 KiB pages, nested paging), an independent one 9.5 ns; link/prof.sh (perf_event sampling) and
  HLINK_TSC=1 (stage clocks) are how these were found. Not yet where it has to be: see STATUS.md.
- 2026-11-13T18:13:08Z Merged the output-side builder (IDEAS 1-3 built): hugetlbfs output (2 MiB layout, chosen when -o is on
  hugetlbfs or with -hugetlb), overwrite of the old output in place (default; -fresh for unlink + create), and
  detached teardown (default: fork, the parent returns when the child says the output is complete;
  -teardown madvise|exit, -nodetach). Its interleaved measurements, MinIO, 8 cores, process ms by the caller's
  clock (hlink as it was this morning: 253): ext4 new file + exit 236, ext4 new + fork 195, ext4 overwrite +
  fork **182**, tmpfs overwrite + fork **173**, hugetlbfs + fork **174**; the write phase goes 39-52 ms (new
  file) -> 14-16 (overwrite) or 18-20 (hugetlbfs: the fill is 3.7 ms, the rest is the copy). Parallel
  MADV_DONTNEED before exit costs 95 ms of kernel CPU: killed. Things learnt about hugetlbfs: every process
  started from it reserves huge pages for its whole image (MinIO: 67 pages) and dies with SIGSEGV after exec
  if the pool is short; the file carries its bss (MinIO 147 MB instead of 109). With hlink's own arrays on
  huge pages too, 256 pages were not enough for link + run: **vm.nr_hugepages is now 1024** (2 GiB).
  Also today: content-addressed symbols are joined after the flood, only the reached ones (1.29 M of 2.68 M
  slots; 457 k classes kept): resolve on one core 152 -> 114 ms, the join itself 37 ms inside deadcode.
