In this repository (bottom-up), the most recent commits changed a phase described in
CHARTER.md and PLAN.md. Check only its correctness against the stock toolchain (a linked binary's behaviour and its
sections against `go tool link`; emitted bytes against the Go assembler): find inputs where its
output differs, especially at chunk and core boundaries and in rarely used symbol kinds. Run the existing differential tests and write new
counterexamples if you find any. Write findings as you go into the output; do not change
implementation or tests. Name each counterexample as a minimal input file.
