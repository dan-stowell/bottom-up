#!/usr/bin/env bash
# The standard library's own tests as programs. For each package, build its test binary twice from the same
# sources (go test -c), linked by the stock linker and by hlink (through -toolexec), run both with -test.short
# in the package's directory, and compare: same exit status and the same set of tests that did not pass.
#   link/stdtest.sh [packages...]     (default: the list below)    results under $HLINK_OUT/std
# Environment: TIMEOUT per test binary in seconds (default 600), CPUS (default 0-3) for taskset.
# The same for any module's packages: MODULE=<module root> link/stdtest.sh <import paths...> (results under
# $HLINK_OUT/<the root's name>; MODFLAGS, default -mod=mod, is added to GOFLAGS). That is how MinIO's and
# kube-apiserver's own tests are run against binaries linked by hlink.
here=$(cd "$(dirname "$0")/.." && pwd)
root=${MODULE:-}; out=${HLINK_OUT:-/tmp/hlink-try}/$([ -n "$root" ] && basename "$root" || echo std); mkdir -p "$out"
pkgs=("$@"); [ ${#pkgs[@]} -gt 0 ] || pkgs=(sort strings strconv fmt reflect runtime sync encoding/json net/http go/types)
export CGO_ENABLED=0 GOFLAGS=-buildvcs=false GOTOOLCHAIN=local
goroot=$(go env GOROOT); pass=0; fail=0
if [ -n "$root" ]; then export GOFLAGS="$GOFLAGS ${MODFLAGS--mod=mod}"; else root=$goroot/src; fi
for p in "${pkgs[@]}"; do
  n=${p//\//__}; d=$(cd "$root" && go list -f '{{.Dir}}' "$p" 2>/dev/null); [ -d "$d" ] || { echo "SKIP $p: no such package"; continue; }
  rm -f "$out/$n.stock" "$out/$n.new"
  if ! (cd "$d" && go test -c -ldflags='-s -w' -o "$out/$n.stock" . ) > "$out/$n.build.log" 2>&1; then
    echo "SKIP $p: stock build failed: $(tail -1 "$out/$n.build.log")"; continue
  fi
  [ -x "$out/$n.stock" ] || { echo "SKIP $p: no tests"; continue; }
  if ! (cd "$d" && go test -c -ldflags='-s -w' -toolexec="$here/link/toolexec.sh" -o "$out/$n.new" . ) > "$out/$n.build.log" 2>&1; then
    echo "FAIL $p: link with hlink failed"; grep -v '^#' "$out/$n.build.log" | head -8; fail=$((fail+1)); continue
  fi
  for w in stock new; do
    (cd "$d" && timeout "${TIMEOUT:-600}" taskset -c "${CPUS:-0-3}" "$out/$n.$w" -test.short -test.v > "$out/$n.$w.out" 2>&1; echo "exit $?" >> "$out/$n.$w.out")
    grep -E '^(--- FAIL|FAIL|panic:|fatal error:|exit )' "$out/$n.$w.out" | sed -E 's/ \([0-9.]+s\)//' | sort > "$out/$n.$w.sum"
  done
  if cmp -s "$out/$n.stock.sum" "$out/$n.new.sum"; then
    echo "PASS $p: $(tail -1 "$out/$n.new.out"), $(grep -c '^--- PASS' "$out/$n.new.out") tests pass, $(grep -c '^--- FAIL' "$out/$n.new.out") fail in both; stock $(stat -c %s "$out/$n.stock") bytes, hlink $(stat -c %s "$out/$n.new") bytes"
    pass=$((pass+1))
  else
    echo "FAIL $p: results differ (stock | hlink)"; diff "$out/$n.stock.sum" "$out/$n.new.sum" | head -10
    grep -m1 -B2 -A12 -E '^(panic:|fatal error:|unexpected fault|SIG)' "$out/$n.new.out" | head -30
    fail=$((fail+1))
  fi
done
echo "$pass packages agree, $fail differ"
[ $fail = 0 ]
