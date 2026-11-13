#!/usr/bin/env bash
# Measure the link limits and keep, per key, the best value seen so far in hw/linklimits.out.txt: every key is
# a rate the machine reached, so on a noisy host the maximum over runs is the limit.   hw/linklimits.sh [runs=1]
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
gcc -O2 -march=native -pthread -o build/linklimits hw/linklimits.c
sed -n 's/^packagefile [^=]*=//p' /home/exedev/targets/frozen/minio/importcfg.link > build/minio.files
for r in $(seq 1 "${1:-1}"); do
  build/linklimits /home/exedev/targets build/minio.files > build/linklimits.run
  touch hw/linklimits.out.txt
  awk 'FNR==NR {v[$1]=$2; o[n++]=$1; next} !($1 in v) {o[n++]=$1} !($1 in v) || $2+0 > v[$1]+0 {v[$1]=$2} END {for (i=0;i<n;i++) print o[i], v[o[i]]}' \
    hw/linklimits.out.txt build/linklimits.run > build/linklimits.merged
  mv build/linklimits.merged hw/linklimits.out.txt
done
cat hw/linklimits.out.txt
