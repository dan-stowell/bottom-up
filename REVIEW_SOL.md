# Strand pool review

Implementation and tests are unchanged. Findings are ordered by severity; passing tests are evidence, not a proof of the policy-independence claim.

## Findings so far

### Chaos does not cover the plain `prev` path (test-coverage / documentation gap)

A line with only `prev` carries takes `fblock`'s mode-0 path directly to `fgood` (`stencils/stub.S:674–680`). In `dojob`, `dj_warm` sends mode 0 straight to `dj_run` (358–361), bypassing both `fs7` and the ignored-carry poisoning in `dj_real0` (458–469). Thus chaos does **not** spoil the ignored parts of every range's `prev` as RUNTIME.md says. This is not by itself an output bug: the single-chunk warm-up can establish the required preceding elements, but that is an additional correctness argument outside the comparison.

For byte `prev`, the comparison conservatively checks the last **dword**, although the stencil reads only byte 63 of the old carry. Chaos preserves that entire last dword, so unused bytes 60–62 are also not tested. This is conservative verification, not an acceptance hole.

## Validation log

- `./build.sh`: PASS, all three stages identical.
- `python3 tests/pool_big.py`: PASS, 6 path checks; 3 programs × 4 inputs × 4 CPU sets.
- `python3 tests/pool.py build/stage2`: PASS, 44 programs, 10,912 runs, 96 path checks.
- `./test.sh 200`: the reference suite and all three stages' case/compile/semantic fuzz suites passed. Three attempts subsequently stopped at `tests/pool.py:86` with `OSError: [Errno 26] Text file busy: /tmp/strand_pool/p`; the first attempt overlapped the standalone pool run, but the subsequent two did not. The remaining commands after the pool checks were run separately, unchanged, and all passed. This is a test-execution limitation, not a program-output mismatch. No harness changes were made.
- Remaining `test.sh` checks: PASS for all 256 four-state / 640 sampled eight-state automata, both linkers, Go lexer (675,686 tokens), lexer fuzz (12 rounds), declaration index (9,185 declarations), and both large compiler inputs (`big8`, `many`). Thus all constituent checks passed across these invocations, although no single uninterrupted `test.sh 200` run completed.

### Reproducer for the unchecked mode-0 path

Program:

```strand
input dup prev - out4
```

Input: `bytes(range(256)) * 4096` (1 MiB). Compile with `build/stage2`, then run pinned to CPU 0, CPUs 0–1, and CPUs 0–1 with arguments `chaos .`. All unmodified runs exit 0 and produce 4,194,304 bytes, SHA-256 `5bd4f8a36b091028bea9b86e40c90bdae193dd9747c25dbabf3232d88311cfe8`.

To establish the path rather than infer it from matching output, patch **copies of the generated ELF**, as `pool_big.py` does: obtain a label offset from `nm build/stub.o`, add 112, and replace 12 bytes there with `b8e7000000bf4d0000000f05` (`exit_group(77)`). With the trap at `fs7`, all three runs still exit 0 with the above output. With the trap at `fgood`, the two multicore runs exit 77 before emitting output; the one-CPU run still succeeds. No repository source or tests were patched.

The qualification matters: carry-free mode 0 obviously needs no comparison; **prev-only mode 0 does read carries**. Its correctness depends on warm-up. Dword `prev` delays one element, byte `prev` one byte; at most eight distinct sum/prev carry depths can appear (`E_CARRY` rejects reuse). Thus a full preceding chunk of 16/64 elements is sufficient for a chain of these delays to forget its initialized carries. This explains why this bypass need not produce wrong output, but contradicts the literal universal `fs7` rule and leaves warm-up outside chaos's claimed protection on this path.

### Byte `prev` can cause unnecessary retries / fallback (performance only)

`fs9` masks mismatching dwords with `0x8000` for both dword and byte `prev` (`stub.S:891–897`). Dword `prev` needs the last dword; byte `prev` needs only the last byte (`tools/mkstencils.py:501–505`). Consequently differing bytes 60–62 can reject an otherwise valid boundary. In chaos these bytes are never spoiled either: `0x7fff` at `stub.S:460` preserves all four bytes of the last dword.

Reproducing program:

```strand
0 1 2 3  1 0 2 3  0 0 0 0 =d
bytes /d dup prev 0 swap %w drop $S
w out4
~1 S + out4
```

Input: 1,048,576 zero bytes, except byte 0 is 1 and byte 262142 is 2. Run on CPUs 0–1 without arguments. At the first boundary, the true previous chunk ends in `[1,1,0,0]`, whereas the guessed chunk ends in `[0,0,0,0]`: the required final byte agrees. A generated-binary `fsretry` exit-77 trap is reached. As a diagnostic control only, replacing the `and esi,0x8000` instruction in that ELF copy with `xor esi,esi` plus padding prevents that trap and preserves the original output. This control is not a proposed fix (it would disable all prev checking). The ordinary program produces correct output (1,048,572 bytes, SHA-256 `074591c892cd76d1558b88c7b43ceb2d51371361b553adff41955c3be038f0f4`); the defect is needless rejection, not false acceptance. The retry chooser at 821–823 has the same over-strict last-dword test.

Additional scratch stress checks used a generated seven-sink byte line occupying carry slots 1–7, alternating dword position sinks (`%`) and byte packing sinks (`^`). One-CPU output matched 2–8 CPUs in normal, eager, chaos, and eager+chaos modes on lengths around 128, 192, 512, 1024, 4096, and 524288, plus 2097153 and 4194305 (large eager cases skipped). These supplement, rather than replace, the repository tests. Tests ran on eight available CPUs; thread counts 9–16 were assessed arithmetically, not executed.

## Answers to the review questions

### 1. Coverage of carries actually read

**Yes, the mode-0 prev-only bypass above exists.** For blocks that actually traverse `fs7`, I found no missing carry bit or unrecorded meaningful input under the current stencil contracts:

- The mask merges sum, prev, and automaton bits onto zmm16–23 and retains max bits for zmm8–15. Iteration maps register index `edx` through `(edx+8)&15`, correctly selecting all 16 carry slots.
- Mode 2 records the `prev` produced by its warm-up into IN before loading/running the range (438–447).
- A skipped one-pass retry restores the IN/W associated with the output it retained (417–423), rather than falsely comparing a newly derived input that it never used.
- Mode 3 records its chosen IN before execution. Subsequent canonicalization changes no compared prev element or automaton state.
- `f_soft` forces fallback before acceptance. `f_bad` accumulates mismatches across all boundaries and is cleared for a fresh attempt, not between boundaries.
- The first range is intentionally not compared against a predecessor: it starts from the saved caller carries. Preserving this input is part of the trusted execution path.

### 2. Comparison by `CARRYKIND`

| Kind | What the body reads / what validation does | Result |
|---|---|---|
| `CK_SUM` | `scan` adds every dword of the input carry; full `vpcmpd` | Correct |
| `CK_MAX` | `scanmax` uses every dword; full `vpcmpd` | Correct |
| `CK_PREV`, dword | `valignd ... ,15` consumes only the last old dword; mask bit 15 | Correct |
| `CK_PREV`, byte | byte shift consumes only byte 63; compares bytes 60–63 | Safe but overly strict, as above |
| `CK_AUTO4`, `CK_AUTO8` | range entry broadcasts byte 0 into the starting xor-permutation (471–477); only that state influences the actual run; compares byte 0 | Correct with this canonicalization |
| `CK_COUNT4`, `CK_COUNT1`, dry-pass mode | bodies extract the low dword, and broadcast the updated count; comparison checks all dwords | Conservative and correct |
| `CK_COUNT4`, `CK_COUNT1`, one-pass mode | counts are deliberate scratch addresses, not serial carry guesses; excluded from `fs7` | Correct only with the separate scratch/copy proof below |

The automaton comparison is a comparison of **state**, not equality of all transition-function bytes. Discarding the rest at range entry is essential. Full-vector equality of sums/maxima is necessary even though ordinary execution produces broadcasts: the stencil adds/maxes lane-wise.

### 3. Scratch arithmetic, copying, fallback, completion

No demonstrated correctness defect found for the supported settings (up to 16 threads, 16/64-element chunks, legal vector lengths):

- `fblock` partitions in 64-element units, hence both chunk sizes fit exactly. A short block goes to `fdone` before running any zero-length range. The final block deliberately leaves at least one serial element, which also repairs/finalizes sink lengths.
- One-pass block length is at most `nthreads * 2^18` and at most `f_blkscan`; rounding only reduces it. Each range therefore has at most `2^18` elements.
- Scratch stride per sink is `0x100040` **bytes**, enough for four bytes per element plus slack. Eight sink slots give `0x800200` bytes per thread. For dword sinks, the thread stride is `0x200080` elements, exactly the byte stride divided by four. `fg2` adds the stride before assigning thread 1, leaving thread 0's unused scratch slot reserved.
- The entire reservation is checked against the heap limit and a distance of `0x7fffffff` from the heap base (591–602). Output vectors are heap allocations below the saved bump pointer; scratch starts above it, aligned to 64 bytes. Thus scratch offsets fit the 32-bit count used by sink stencils, and dword offsets divide exactly by four.
- `fc2` takes each range's local length as OUT minus its scratch baseline, then forms the source address in the correct byte/dword units. Its destination is the prefix total after preceding ranges. Prefix accumulation is across **all** ranges, independent of their scratch offsets. Copy destinations are below the bump pointer; scratch sources are above it. Parallel copies therefore do not clobber their sources or each other's destination intervals.
- `fcopied` restores the last range's non-count carries and replaces scratch counts with the true totals. Copy mode changes no vector accumulators. `fgood` folds workers into the caller's accumulators, excluding the caller's already-resident slot.
- `fsbad` restores the first range's true carries and the pre-block sum/max accumulators. It redoes the complete block serially, or returns the remaining line to the serial caller. Existing partial results are overwritten; illegal speculative memory operations are separately guarded/masked by the stencils. Scatter/slice operations veto the pool, so they do not require rollback here.
- `fdone` restores the saved bump pointer, output pointer/flush limit, loop position, range mask indices and serial continuation.

These are arithmetic/allocator/stencil obligations, **not consequences of the carry comparison**. I did not find a wraparound, unit mismatch, source/destination overlap, or accumulator double-fold in them. Arbitrarily patching block sizes beyond the scratch-cap invariant is not a supported configuration.

### 4. Fairness and limits of chaos

Chaos is useful evidence for wrong guessed/derived states, retry decisions, and reduced prev comparisons **on the checked paths**, but not a proof of the universal claim:

- It entirely misses mode-0 prev-only execution, as demonstrated.
- Its byte-prev poisoning does not test unused bytes 60–62; validation also unnecessarily compares them.
- It leaves the first range's meaningful carries and all sink counts untouched by design. The scratch/count derivation and memory safety must be justified separately; the exemption is not evidence they are correct.
- Noise is structured (one broadcast dword per chosen range), not arbitrary independent corruption of all carry bytes. Dry warm-ups and automaton canonicalization are not independently destroyed. This is a bounded fault model, not exhaustive quantification over every possible replacement of the policy code.
- Automaton state-only checking depends on the trusted range-entry broadcast; the tests do not establish correctness if that step itself is changed.

I found **no wrong-output reproducer in the unmodified implementation** during this review. The concrete findings are a violated universal validation/chaos claim and conservative byte-prev validation that can waste retries.
