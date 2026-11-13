You are reviewing the bottom-up repository (current directory), working toward the goal in
CHARTER.md: data-parallel kernels at hardware speed, grown rung by rung into a Go front end. Read
CHARTER.md, PLAN.md, STATUS.md, FLOOR.md and the tail of JOURNAL.md. Do not edit code or tests.

Answer briefly and concretely, citing files and numbers:
1. Which rung's floor in FLOOR.md is wrong or optimistic, and what is the real limit?
2. What keeps the current rung from its floor (a serial dependency, bytes moved twice, a scalar
   fallback), and is the agent attacking it?
3. What is the strongest reason the current design will not scale to the next rungs, and what
   would you build instead?
4. Where does the oracle (go/scanner, go/parser, go/types) not actually check the output?
5. Quote any recent JOURNAL.md passage that declines or shrinks work because of time, usage or a
   session ending. Did PLAN.md's oracle number move since the last milestone? What is the largest
   blocker in the campaign that is not being attacked?
Skip praise and skip auditing benchmark claims.
