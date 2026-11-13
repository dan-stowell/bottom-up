# STATUS (at most ten lines; updated at every milestone and whenever Dan is needed)

2026-11-13T18:24:13Z (stopped on request). Campaign 1. M0-M3 reached: 5 programs, 259/259 std test binaries, MinIO's 46 test
packages agree with the stock link (to be rerun on the final linker); kube-apiserver 4 of 18 so far, 0 differ; secdiff
(symbolic section diff vs go tool link) exists; a builder's fixes for it are on an unmerged branch.
Speed, MinIO, 8 cores: floor 9.7 ms (model in FLOOR.md); stock 1,750 ms; fclink 303; hlink 251 this morning, **167-182
now**, about 5% of floor; kube-apiserver 267. All of today's 8-core numbers were taken on a busy machine (FLOOR.md's
243 ms MinIO row is a disturbed run). Projection for this design on stock objects: 35-45 ms: above the kill line,
short of the exit line. Default in-place overwrite of the output is unsafe against a concurrent link (fix in flight; -fresh avoids it).

## Decisions for Dan

- Scope is `-s -w`, static linux/amd64, CGO off (as fclink). The linker is C, not Strand (JOURNAL, first entry).
- The exit line needs more than a single-shot link of stock objects, by my numbers: 4 KiB pages make a
  dependent cache miss 158 ns here and mapping 456 MB of object metadata 20 ms of kernel time. Candidates
  that need your say: a per-archive sidecar index cached between links; inputs on hugetlbfs.
- vm.nr_hugepages is set to 1024 on this VM (not persistent across reboots); hlink uses them when present.
