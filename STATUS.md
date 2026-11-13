# STATUS (at most ten lines; updated at every milestone and whenever Dan is needed)

2026-11-13T17:23:30Z. Campaign 1, M0 nearly done: floors computed (FLOOR.md): hardware floor on 8 cores is 6.9 ms for MinIO, 11.7 ms for kube-apiserver.
Oracle number: 0 programs linked our way yet. Baselines here: stock 1,750 ms, fclink 303 ms (2.3% of floor); kill line 69 ms, exit line 21 ms.
In flight: hlink (C port of fclink's design) and secdiff (symbolic section diff vs go tool link), two builders.

## Decisions for Dan

- Scope is `-s -w` (no DWARF/symtab), static linux/amd64, CGO off, like fclink. Say if DWARF must be in.
- The linker is C, not Strand (JOURNAL.md, first entry, has the reasoning). Say if it must be Strand.
- The kernel binds: creating the 110 MB output costs 31-39 ms on ext4 (13 ms when overwriting a cached file) against a 6.9 ms hardware
  floor (PLAN.md's estimate was "tens of ms": measured exactly it is lower, so the exit line is harder). FLOOR.md keeps OS limits as separate rows; the exit test is read against the hardware floor.
