#!/bin/sh
# Build everything: stencils -> stage0 (Python) -> stage1 -> stage2 -> stage3, and check the fixed point.
set -e
cd "$(dirname "$0")"
python3 tools/mkstencils.py
cat src/tables.strand src/compiler.strand > build/strandc.strand
python3 boot/strand.py compile build/strandc.strand > build/stage1
chmod +x build/stage1
./build/stage1 < build/strandc.strand > build/stage2
chmod +x build/stage2
./build/stage2 < build/strandc.strand > build/stage3
chmod +x build/stage3
cmp build/stage1 build/stage2 && echo "stage1 == stage2 (SIMD compiler reproduces the scalar reference)"
cmp build/stage2 build/stage3 && echo "stage2 == stage3 (fixed point)"
sha256sum build/stage1 build/stage2 build/stage3
