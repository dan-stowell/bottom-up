#!/usr/bin/env bash
# The standard library's own tests as programs. For each package, build its test binary twice from the same
# sources (go test -c), linked by the stock linker and by hlink (through -toolexec), run both with -test.short
# in the package's directory, and compare: same exit status and the same set of tests that did not pass.
#   link/stdtest.sh [packages...]     (default: the list below)    results under $HLINK_OUT/std
# Environment: TIMEOUT per test binary in seconds (default 600), CPUS (default 0-3) for taskset.
# HLINK_HUGETLB=1: hlink writes each test binary on hugetlbfs (through link/toolexec.sh, which then copies it to
# where the go command wants it), and the hlink binary is run from hugetlbfs: copied back into HUGETLB_DIR
# (default /dev/hugepages) through a shared mapping (hugetlbfs has no write()), and removed after the run.
here=$(cd "$(dirname "$0")/.." && pwd)
out=${HLINK_OUT:-/tmp/hlink-try}/std; mkdir -p "$out"
pkgs=("$@"); [ ${#pkgs[@]} -gt 0 ] || pkgs=(sort strings strconv fmt reflect runtime sync encoding/json net/http go/types)
export CGO_ENABLED=0 GOFLAGS=-buildvcs=false GOTOOLCHAIN=local
goroot=$(go env GOROOT); pass=0; fail=0
hugecp() { # hugecp src dst: copy a file into hugetlbfs
  python3 -I -c 'import mmap, os, sys
d = open(sys.argv[1], "rb").read(); fd = os.open(sys.argv[2], os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o755)
os.ftruncate(fd, len(d)); m = mmap.mmap(fd, len(d), mmap.MAP_SHARED); m[:] = d; m.close(); os.close(fd)' "$1" "$2"
}
for p in "${pkgs[@]}"; do
  n=${p//\//__}; d=$goroot/src/$p
  rm -f "$out/$n.stock" "$out/$n.new"
  if ! (cd "$d" && go test -c -ldflags='-s -w' -o "$out/$n.stock" . ) > "$out/$n.build.log" 2>&1; then
    echo "SKIP $p: stock build failed: $(tail -1 "$out/$n.build.log")"; continue
  fi
  [ -x "$out/$n.stock" ] || { echo "SKIP $p: no tests"; continue; }
  if ! (cd "$d" && go test -c -ldflags='-s -w' -toolexec="$here/link/toolexec.sh" -o "$out/$n.new" . ) > "$out/$n.build.log" 2>&1; then
    echo "FAIL $p: link with hlink failed"; grep -v '^#' "$out/$n.build.log" | head -8; fail=$((fail+1)); continue
  fi
  huge=""
  if [ "${HLINK_HUGETLB:-}" = 1 ]; then
    # (the go command could hand back a cached link from a run without HLINK_HUGETLB: check the layout)
    readelf -lW "$out/$n.new" | grep -q '^ *LOAD .* 0x200000$' || { echo "FAIL $p: the hlink binary is not laid out for hugetlbfs"; fail=$((fail+1)); continue; }
    huge=${HUGETLB_DIR:-/dev/hugepages}/hlink-stdtest.$n.$$
    hugecp "$out/$n.new" "$huge" || { echo "FAIL $p: copy into $huge failed"; rm -f "$huge"; fail=$((fail+1)); continue; }
  fi
  for w in stock new; do
    bin=$out/$n.$w; [ $w = new ] && [ -n "$huge" ] && bin=$huge
    (cd "$d" && timeout "${TIMEOUT:-600}" taskset -c "${CPUS:-0-3}" "$bin" -test.short -test.v > "$out/$n.$w.out" 2>&1; echo "exit $?" >> "$out/$n.$w.out")
    grep -E '^(--- FAIL|FAIL|panic:|fatal error:|exit )' "$out/$n.$w.out" | sed -E 's/ \([0-9.]+s\)//' | sort > "$out/$n.$w.sum"
  done
  [ -n "$huge" ] && rm -f "$huge"
  if cmp -s "$out/$n.stock.sum" "$out/$n.new.sum"; then
    echo "PASS $p: $(tail -1 "$out/$n.new.out"), $(grep -c '^--- PASS' "$out/$n.new.out") tests pass, $(grep -c '^--- FAIL' "$out/$n.new.out") fail in both; stock $(stat -c %s "$out/$n.stock") bytes, hlink $(stat -c %s "$out/$n.new") bytes${huge:+, run from hugetlbfs}"
    pass=$((pass+1))
  else
    echo "FAIL $p: results differ (stock | hlink)"; diff "$out/$n.stock.sum" "$out/$n.new.sum" | head -10
    grep -m1 -B2 -A12 -E '^(panic:|fatal error:|unexpected fault|SIG)' "$out/$n.new.out" | head -30
    fail=$((fail+1))
  fi
done
echo "$pass packages agree, $fail differ"
[ $fail = 0 ]
