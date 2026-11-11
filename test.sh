#!/bin/sh
# Full suite, on every bootstrap stage. A fixed point proves consistency, not correctness;
# this is what argues correctness.   ./test.sh [N]   (N = fuzz iterations per stage, default 400)
set -e
cd "$(dirname "$0")"
N=${1:-400}
./build.sh
echo "--- stencils vs scalar interpreter (reference compiler emits, interpreter judges)"
python3 tests/fuzz_sem.py --n "$N" --seed 11
for s in stage1 stage2 stage3; do
  echo "--- $s"
  python3 tests/cases.py build/$s
  python3 tests/fuzz_compile.py --compiler build/$s --also build/strandc_scalar --n "$N" --seed 7
  python3 tests/fuzz_sem.py --compiler build/$s --n "$N" --seed 5
done
echo "--- thread pool at production block sizes, 2..all CPUs, file and pipe"
python3 tests/pool.py build/stage2
python3 tests/pool_big.py
echo "--- all 256 four-state transition functions"
python3 tests/fsm_all.py build/stage2
echo "--- a linker written in Strand against the C reference (explore/linkjob.c), both ways of resolving imports"
gcc -O2 -o build/linkjob explore/linkjob.c && ./build/linkjob gen 40 > build/job40
for l in link link2; do
  ./build/stage2 < examples/$l.strand > build/$l && chmod +x build/$l
  ./build/$l < build/job40 > build/$l.out; ./build/linkjob ref < build/job40 | cmp - build/$l.out
  echo "PASS $l"
done
echo "--- a Go lexer written in Strand against go/scanner, on part of the Go standard library, one CPU and all"
if command -v go >/dev/null; then
  (cd explore/golex && go build -o ../../build/golex .)
  find "$(go env GOROOT)/src/go" "$(go env GOROOT)/src/strconv" "$(go env GOROOT)/src/math" -name '*.go' -type f | sort > build/gofiles.small
  ./build/golex corpus build/gofiles.small > build/gosmall.go 2>/dev/null
  ./build/golex dump build/gosmall.go > build/gosmall.tok 2>/dev/null
  python3 examples/golex_gen.py | cmp - examples/golex.strand
  ./build/stage2 < examples/golex.strand > build/golex_strand && chmod +x build/golex_strand
  taskset -c "$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')" ./build/golex_strand < build/gosmall.go | cmp - build/gosmall.tok
  ./build/golex_strand < build/gosmall.go | cmp - build/gosmall.tok
  ./build/golex_strand chaos . < build/gosmall.go | cmp - build/gosmall.tok      # the pool's guesses spoiled: same tokens
  echo "PASS golex ($(($(stat -c %s build/gosmall.tok) / 5)) tokens)"
  python3 tests/golex_fuzz.py 12
  # package-level declarations (nesting by a running sum, an automaton over the tokens) against go/parser
  { echo tests/go/decls.go; cat build/gofiles.small; } > build/gofiles.decl
  ./build/golex pcorpus build/gofiles.decl > build/godecl.go 2>/dev/null
  ./build/golex decls build/gofiles.decl > build/godecl.ref
  python3 examples/godecl_gen.py | cmp - examples/godecl.strand
  ./build/stage2 < examples/godecl.strand > build/godecl && chmod +x build/godecl
  taskset -c "$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')" ./build/godecl < build/godecl.go | cmp - build/godecl.ref
  ./build/godecl < build/godecl.go | cmp - build/godecl.ref
  ./build/godecl chaos . < build/godecl.go | cmp - build/godecl.ref
  echo "PASS godecl ($(($(stat -c %s build/godecl.ref) / 5)) declarations)"
else echo "SKIP golex: no go toolchain"; fi
echo "--- big inputs: the three compilers agree"
for f in big8 many; do
  [ $f = big8 ] && python3 tools/mkbig.py 8 > build/big8.strand
  [ $f = many ] && python3 -c "import sys; sys.stdout.write(''.join('1 >X%d\n' % i for i in range(40000)))" > build/many.strand
  ./build/stage2 < build/$f.strand > build/$f.simd
  ./build/strandc_scalar < build/$f.strand | cmp - build/$f.simd
  python3 boot/strand.py compile build/$f.strand | cmp - build/$f.simd
  echo "PASS $f"
done
echo "ALL PASS"
