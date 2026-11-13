# PLAN

The campaign is chosen by the user. Sessions **do not edit** the Campaign, Exit test, Kill
test or Rules sections; they append to Milestones and keep the Now line current. If you think
the campaign is wrong, write the evidence in STATUS.md under "Decisions for Dan" and keep
working on it.

## Campaign 1 (2026-11-13): linking at hardware speed

Turn MinIO's real object files into a binary that runs and passes MinIO's tests, in one pass
over all cores, as close to the machine's floor as you can get. The stock linker takes 1,180 ms,
mostly serial; simd-lang's `fclink` takes 330 ms. The floor is set by bytes: about 0.5 GB of
objects in and 0.2 GB of binary out, against 43 GB/s of memory bandwidth on 8 cores
(HARDWARE.md), so tens of milliseconds. Compute the floor exactly at M0.

The phases, each data-parallel-shaped: symbol resolution (a hash join over about 1.8M
symbols, measured as memory-latency bound), reachability (a graph traversal), layout (a prefix
sum over sizes), relocation (a scatter), pc tables and the other runtime metadata (parallel
encoding of offset streams), and writing straight into the image. Reshape anything that looks
serial before accepting it. How is yours to decide; simd-lang's `explore/f4link` spike and
`fclink` are there to read and borrow from.

Input: frozen stock objects (`go build -a -work` with the stock toolchain, kept under
/home/exedev/targets/) for MinIO, then kube-apiserver, so this doesn't depend on `fc`.

## Oracle number (report it at every milestone)

Programs whose binary, linked our way, passes its tests (std's own test binaries first, then
MinIO, then kube-apiserver); and for each, the link's ms on 8 cores against its floor.

## Kill test

If the best design measures below 10% of the floor on 8 cores for MinIO, with no measured path
above that, write up why and ask under "Decisions for Dan". Time, usage and session ends never
end the campaign.

## Exit test

MinIO and kube-apiserver linked our way pass their tests, at a third of the floor or better
(within 3x of it) on 8 cores.

## Campaign 2 (next, when Dan says): code emission at hardware speed

Lowered instructions to machine code: table-driven encoding, branch sizing as a fixed point of
prefix sums instead of re-assembling (the stock assembler assembles each function 3.3 times),
stack maps and pc tables in the same pass. Oracle: the Go assembler's bytes for the same input.

## Rules

- Commit and push every working step, with tests for what it touches.
- Use subagents as builders: split work into oracle-checked parts, one git worktree each.
- The 8-vCPU VM is the machine. Report 1, 2, 4 and 8 cores for every workload.

## Milestones

M0. The floor, computed from HARDWARE.md and `hw/` for each phase; frozen stock objects for MinIO
    and kube-apiserver; `FLOOR.md` and its script; the oracle harness (link, run the tests, diff
    sections against `go tool link`).
M1. A small program (hello) linked our way runs.
M2. Standard-library test binaries linked our way pass.
M3. MinIO linked our way passes its tests.
M4. Within 3x of the floor on MinIO; kube-apiserver passes.

Log (append one line per milestone reached, with the oracle number):

- 2026-11-13T17:47:38Z M0: floors (MinIO 6.9 ms, kube-apiserver 11.7 ms hardware; FLOOR.md), frozen objects for both, bench + floor scripts. Oracle harness: link/try.sh, link/stdtest.sh; secdiff still being built.
- 2026-11-13T17:47:38Z M1: hello and 4 other programs linked by hlink run like the stock link (link/test.sh). Oracle number: 5 programs.
- 2026-11-13T17:47:38Z M2: all 259 standard-library packages that have tests and build without cgo: test binaries linked by hlink give the stock link's results (link/stdtest.sh over `go list std`). MinIO (not yet its tests): 188 ms on 8 cores before any tuning, 3.7% of floor.
- 2026-11-13T18:19:53Z M3: MinIO's own tests: all 46 packages with tests give the stock link's results when linked by hlink (`MODULE=/home/exedev/targets/minio link/stdtest.sh ...`; `cmd`: 710 pass, 1 fails in both). Oracle number: 5 programs, 259 std test binaries, 46 MinIO test binaries. MinIO link on 8 cores: 173-182 ms process (builder's interleaved minima), about 4-5% of floor.

Now: Fresh context taken at 2026-11-13T18:25Z with work in flight; pick it up in this order. (1) Two builders were running in worktrees: `worktree-agent-a2090067627221bc0` (make `link/secdiff.sh` exit 0 on hlink: -dumpmap contract, stock load order for DUPOK winners, alignment of merged and zero-size symbols) and `worktree-agent-a1f48af780dd56e0f` (output claimed by rename at start, written under a temporary name, renamed into place; completion byte after the child lets go of stdio). If a task notification with their report arrives, act on it; otherwise look at `git log link-hardware-speed..<branch>` and the worktree under `.claude/worktrees/`: merge a branch only when its last commit says the work is done and `link/test.sh` passes after the merge (resolve conflicts by hand: I changed link/resolve.c, link/dead.c, link/load.c, link/common.h, link/test.sh since they branched). If a branch looks abandoned midway, start a new builder from its commits with the same brief (JOURNAL.md has what each was for). Still to hand to the fidelity builder or a new one (REVIEWS.md, codex-front-half 3 and 4): a BSS declaration larger than the initialised definition it loses to must still size the symbol (stock loader's size fixups); the stock loader loads all package-indexed definitions before the others, which changes equal-rank ties. (2) `/tmp/hlink-try/kubetests.log`: kube-apiserver's 18 test packages, hlink against stock (`MODULE=/home/exedev/targets/kubernetes MODFLAGS= TIMEOUT=1200 link/stdtest.sh $(cat /tmp/hlink-try/kube.testpkgs)`; list from `go list -f '{{if or .TestGoFiles .XTestGoFiles}}{{.ImportPath}}{{end}}' ./cmd/kube-apiserver/... ./pkg/controlplane/... ./pkg/kubeapiserver/...` in /home/exedev/targets/kubernetes); record the result in JOURNAL.md. Before M4 is claimed rerun on the final linker: that, MinIO's 46 (`/tmp/hlink-try/minio.testpkgs`, `MODULE=/home/exedev/targets/minio`), and all of std (`link/stdtest.sh $(cd /usr/local/go/src && go list std | grep -v -E '^(vendor|cmd)/')`). (3) Triage `reviews/inbox/` (a codex skeptic review was fired at M3) into REVIEWS.md. (4) Speed. Measure only when `uptime` shows load under 1 (today's 8-core numbers were taken with builders and test suites running and are noise; FLOOR.md's MinIO 8-core row of 243 ms is one such): `COLD=1 link/bench.sh minio build/hlink 7 && COLD=1 link/bench.sh kube-apiserver build/hlink 3 && python3 link/floor.py hlink > FLOOR.md`. Tools: `HLINK_TSC=1 build/hlink -stats -nodetach -fresh -j 1 ...` (stage clocks), `link/prof.sh minio -j 1`. Single-core ms now: load 36, resolve 100, deadcode 136 (floods 90, content-hash join 33), layout 74, create 26, write 97. Next steps, in order: (a) layout over live symbols only (IDEAS.md 11; link/layout.c sizeUnit/addrUnit/otherSlots: test liveness on FL/SF bytes, keep sizes from the first pass, cache function shapes for write); (b) write: no zero pass on overwrite (have each unit write its own padding), relocation targets prefetched in batches like the flood; (c) check the flood's scaling on a quiet machine (adaptive grain is in; the spin-then-futex pool collapses when the machine is oversubscribed: load 20 made the 8-thread flood 6x slower); (d) IDEAS.md 9 (inputs on hugetlbfs) as an experiment. Kill line 10% of floor (floor is 9.7 ms for MinIO in FLOOR.md's model), exit line 33%.
