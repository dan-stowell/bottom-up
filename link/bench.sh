#!/usr/bin/env bash
# Time a linker on a frozen workload (link/freeze.sh) on 1, 2, 4 and 8 cores, warm and cold.
#   link/bench.sh <workload> [linker=build/hlink] [rounds=7]     -> link/results/<workload>.<linker name>.tsv
# Rows: "<cores> <phase> <ms>" from the linker's -stats table of its fastest run on that many cores (taskset
# and -j both set), plus phase "process" (the whole process by the caller's clock, start to exit: the minimum
# over the rounds) and, for 8 cores, "cold" (process time after dropping the page cache; the output directory
# stays). Core counts are interleaved within a round so that drift on this noisy host hits all of them alike.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
w=$1; linker=${2:-$here/build/hlink}; rounds=${3:-7}
d=${FROZEN:-/home/exedev/targets/frozen}/$w; out=${HLINK_OUT:-/tmp/hlink-try}/bench; mkdir -p "$out" "$here/link/results"
mapfile -t F < "$d/link.flags"
res=$here/link/results/$w.$(basename "$linker").tsv
run() { # run <cores> -> prints process ms; stats in $out/stats
  local j=$1 t0 t1
  t0=$(date +%s%N)
  taskset -c 0-$((j-1)) "$linker" -stats -j "$j" -o "$out/$w.out" -importcfg "$d/importcfg.link" "${F[@]}" -s -w "$d/main.a" > "$out/stats"
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000 ))
}
declare -A best beststats
for r in $(seq 1 "$rounds"); do
  for j in 8 1 4 2; do
    us=$(run $j)
    tot=$(awk '$1=="total"{print int($2*1000)}' "$out/stats")
    if [ -z "${best[$j]:-}" ] || [ "$us" -lt "${best[$j]}" ]; then best[$j]=$us; fi
    if [ -z "${beststats[$j]:-}" ] || [ "$tot" -lt "${beststats[$j]%% *}" ]; then beststats[$j]="$tot $(awk '{printf "%s:%s;", $1, $2}' "$out/stats")"; cp "$out/stats" "$out/stats.$j"; fi
  done
done
cold=""
for r in 1 2 3; do
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
  us=$(run 8); if [ -z "$cold" ] || [ "$us" -lt "$cold" ]; then cold=$us; fi
done
{
  for j in 1 2 4 8; do
    awk -v j=$j '$1!="phase" && $2 ~ /^[0-9.]+$/ {print j "\t" $1 "\t" $2}' "$out/stats.$j"
    echo -e "$j\tprocess\t$(awk -v u=${best[$j]} 'BEGIN{printf "%.1f", u/1000}')"
  done
  echo -e "8\tcold\t$(awk -v u=$cold 'BEGIN{printf "%.1f", u/1000}')"
  # what the link was made of, from the fastest 8-core run (the notes of the -stats table)
  sed 's/^/# /' "$out/stats.8"
  echo "# output_bytes $(stat -c %s "$out/$w.out")"
  echo "# measured $(date -u +%FT%TZ), $rounds rounds, $(basename "$linker")"
} > "$res"
cat "$res"
