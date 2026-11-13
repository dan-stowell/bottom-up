#!/usr/bin/env bash
# Build hlink: link/build.sh  ->  build/hlink   (link/gotables.h is checked in; link/gen/gen.sh regenerates it)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/../build"
gcc -O2 -g -march=native -pthread -std=gnu11 -Wall -Wextra -Wno-sign-compare -Wno-missing-field-initializers \
  -o "$here/../build/hlink" "$here/hlink.c" ${HLINK_CFLAGS:-}
echo "built $(cd "$here/.." && pwd)/build/hlink"
