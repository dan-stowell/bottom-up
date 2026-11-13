# STATUS (at most ten lines; updated at every milestone and whenever Dan is needed)

2026-11-13T18:13:33Z. Campaign 1. M0-M2 reached; M3 nearly (45 of MinIO's 46 test packages agree with the stock link,
`cmd` still running). Oracle: 5 programs, 259/259 std test binaries, MinIO --version + 45 packages; secdiff
(symbolic section diff vs go tool link) exists and is being driven to zero differences by a builder.
Speed, MinIO, 8 cores: hardware floor 6.9 ms; stock 1,750 ms; fclink 303; hlink 251 this morning, **173-182
now** (overwrite-in-place or hugetlbfs output, detached teardown); front half rewritten, not yet measured on a
quiet machine. Honest projection for this design on stock objects: 35-45 ms (15-20% of floor), above the
kill line (69 ms) and short of the exit line (21 ms).

## Decisions for Dan

- Scope is `-s -w`, static linux/amd64, CGO off (as fclink). The linker is C, not Strand (JOURNAL, first entry).
- The exit line needs more than a single-shot link of stock objects, by my numbers: 4 KiB pages make a
  dependent cache miss 158 ns here and mapping 456 MB of object metadata 20 ms of kernel time. Candidates
  that need your say: a per-archive sidecar index cached between links; inputs on hugetlbfs.
- vm.nr_hugepages is set to 1024 on this VM (not persistent across reboots); hlink uses them when present.
