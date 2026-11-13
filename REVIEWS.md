# REVIEWS: triage of reviews/inbox (one line each)

- 20261113 codex-m0-floor, copy bandwidth counts payload once but floor divided reads+writes by it: **accept**. Measured read (92.5 GB/s) and non-temporal write (56.6 GB/s) streams separately; floor = in/read + out/write. MinIO floor 13.2 -> 6.9 ms.
- same, layout charged for dead slots: **accept** (live symbols only).
- same, reach uses applied relocations as the edge count: **accept, open**: needs the traversed-edge count from hlink's -stats; noted in the floor's legend until then.
- same, pointer chase was not one cycle: **accept** (Sattolo permutation; chase_64m 169 M/s -> 8.5 M/s; not used by a floor yet).
- same, random-load index stream was serially dependent: **accept** (counter through a multiplicative hash; rand_16m_1t 316 -> 530 M/s).
- same, freeze.sh kept an old main.a / stale objs and split quoted flags: **accept** (clears first; shlex).
- same, bench.sh subtracted minima of different runs: **accept** (phase table now comes from the run with the fastest process).
- 20261113 codex-hlink-port (9 findings). 1 (u32 size sums wrap for >4 GB images), 2 (GC bitmap index not bounded), 3 (symbol index not bounded in oref), 4 (index tables not validated at load), 5 (FuncInfo length), 6 (R_METHODOFF sentinel writes 4 bytes whatever the declared size), 7 (typeString varint): **defer as one item**: all need a malformed or >4 GB object, which the stock compiler does not write; fclink has the same holes. To do once the phases stop moving: one validation pass at load (index tables monotonic and in range, symbol indices in range), then the accessors can stay unchecked.
- same, 8 (plain read of state[] racing with atomic or): **accept**, fix in the reachability rewrite.
- same, 9 (iswupper uses libc's Unicode tables, Go uses Unicode 17): **accept**: generate the upper-case ranges from Go's unicode package in link/gen/mktables.go.
