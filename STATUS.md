# STATUS (at most ten lines; updated at every milestone and whenever Dan is needed)

2026-11-13T17:23:30Z. Campaign 1, M0 nearly done: floor computed (FLOOR.md): MinIO hardware floor 13.2 ms on 8 cores.
Oracle number: 0 programs linked our way yet. Baselines here: stock 1,750 ms, fclink 295 ms (4.5% of floor).
In flight: hlink (C port of fclink's design) and secdiff (symbolic section diff vs go tool link), two builders.

## Decisions for Dan

- Scope is `-s -w` (no DWARF/symtab), static linux/amd64, CGO off, like fclink. Say if DWARF must be in.
- The linker is C, not Strand (JOURNAL.md, first entry, has the reasoning). Say if it must be Strand.
- The kernel binds: creating the 110 MB output costs 34 ms on ext4 (14 ms on tmpfs) against a 13 ms hardware
  floor. FLOOR.md keeps OS limits as separate rows; the exit test is read against the hardware floor.
