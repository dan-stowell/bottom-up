# REVIEWS: triage of reviews/inbox (one line each)

- 20261113 codex-m0-floor, copy bandwidth counts payload once but floor divided reads+writes by it: **accept**. Measured read (92.5 GB/s) and non-temporal write (56.6 GB/s) streams separately; floor = in/read + out/write. MinIO floor 13.2 -> 6.9 ms.
- same, layout charged for dead slots: **accept** (live symbols only).
- same, reach uses applied relocations as the edge count: **accept, open**: needs the traversed-edge count from hlink's -stats; noted in the floor's legend until then.
- same, pointer chase was not one cycle: **accept** (Sattolo permutation; chase_64m 169 M/s -> 8.5 M/s; not used by a floor yet).
- same, random-load index stream was serially dependent: **accept** (counter through a multiplicative hash; rand_16m_1t 316 -> 530 M/s).
- same, freeze.sh kept an old main.a / stale objs and split quoted flags: **accept** (clears first; shlex).
- same, bench.sh subtracted minima of different runs: **accept** (phase table now comes from the run with the fastest process).
