In this repository (bottom-up), the most recent commits added or changed a rung described in
CHARTER.md and PLAN.md. Check only its correctness against the oracle (go/scanner, go/parser or
go/types, as the rung says): find inputs where its output differs, especially at chunk and core
boundaries and in rarely used syntax. Run the existing differential tests and write new
counterexamples if you find any. Write findings as you go into the output; do not change
implementation or tests. Name each counterexample as a minimal input file.
