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

Now: Start M0: compute each phase's floor (HARDWARE.md, hw/), freeze stock objects for MinIO, write FLOOR.md's script and the link oracle.
