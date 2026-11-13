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
