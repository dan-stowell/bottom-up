# PLAN

The campaign is chosen by the user. Sessions **do not edit** the Campaign, Exit test, Kill
test or Rules sections; they append to Milestones and keep the Now line current. If you think
the campaign is wrong, write the evidence in STATUS.md under "Decisions for Dan" and keep
working on it.

## Campaign 1 (draft, 2026-11-13): Go's front end at hardware speed

Grow Strand from kernels into a Go front end, one rung at a time, each at a measured fraction
of its floor and checked by the stock oracle on all of MinIO and the standard library:

1. **Tokens**: the same token stream as `go/scanner`. Strand's existing Go lexer
   (`examples/golex.strand`, GOLEX.md) is the start.
2. **Trees**: the same syntax trees as `go/parser`. Parsing is the first rung that is not
   obviously data-parallel; reshape it (for example, bracket matching by prefix sums, then
   declarations and statements as independent spans) before giving up on it.
3. **Names**: every identifier resolved to its declaration, the same as `go/types`.
4. **Types**: the types `go/types` computes, for the declarations `fc` needs.

How is yours to decide.

## Oracle number (report it at every milestone)

For each rung: the fraction of the corpus (MinIO plus the standard library, by bytes) on which
the oracle agrees, and its % of floor on 8 cores.

## Kill test

If a rung's best design measures below 10% of its floor on 8 cores, with no measured path
above that, write up why and ask for a decision under "Decisions for Dan" before the next
rung. Time, usage and session ends never end the campaign.

## Exit test

All four rungs agree with the oracle on the whole corpus, and `fc` (in simd-lang) takes its
front end from this project for a MinIO build that passes its tests.

## Rules

- Commit and push every working step, with tests for what it touches.
- Use subagents as builders: split work into oracle-checked parts, one git worktree each.
- The 8-vCPU VM is the machine. Report 1 and 8 cores for every rung.

## Milestones

M0. `FLOOR.md` and its script; the corpus fetched and pinned (MinIO's sources and the
    standard library); the oracle harness for rung 1.
M1. Rung 1 (tokens) on the whole corpus.
M2. Rung 2 (trees).
M3. Rung 3 (names).
M4. Rung 4 (types), consumed by `fc`.

Log (append one line per milestone reached, with the oracle number):

Now: Start M0: measure the floors (hw/), fetch and pin the corpus, write FLOOR.md's script and the token oracle.
