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
  python3 tests/fuzz_compile.py --compiler build/$s --n "$N" --seed 7
  python3 tests/fuzz_sem.py --compiler build/$s --n "$N" --seed 5
done
echo "ALL PASS"
