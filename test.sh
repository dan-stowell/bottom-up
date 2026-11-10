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
echo "--- big inputs: the three compilers agree"
for f in big8 many; do
  [ $f = big8 ] && for k in 1 2 3 4 5 6 7 8; do cat build/strandc.strand; done > build/big8.strand
  [ $f = many ] && python3 -c "import sys; sys.stdout.write(''.join('1 >X%d\n' % i for i in range(40000)))" > build/many.strand
  ./build/stage2 < build/$f.strand > build/$f.simd
  ./build/strandc_scalar < build/$f.strand | cmp - build/$f.simd
  python3 boot/strand.py compile build/$f.strand | cmp - build/$f.simd
  echo "PASS $f"
done
echo "ALL PASS"
