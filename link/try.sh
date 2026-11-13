#!/usr/bin/env bash
# The hlink oracle for one program: build a Go main package with the stock toolchain, link the very same objects
# again with hlink, run both binaries with the same arguments and compare output and exit status.
#   link/try.sh <dir of a Go main package> [program arguments...]
# Environment: HLINK (default build/hlink), GCFLAGS (default -gcflags=all=-l; set to a space for the default
# optimisations, i.e. with inlining), HLINK_OUT (default /tmp/hlink-try), TIMEOUT (60), HLINK_FLAGS (extra
# flags for hlink), KEEP_WORK (keep the build's objects).
# Addresses differ between the two links, so hex numbers are masked before comparing (tracebacks).
here=$(cd "$(dirname "$0")/.." && pwd)
hlink=${HLINK:-$here/build/hlink}
dir=$(cd "$1" && pwd) || exit 2; shift
out=${HLINK_OUT:-/tmp/hlink-try}/$(basename "$dir"); mkdir -p "$out"; rm -f "$out/stock" "$out/new"
log=$(cd "$dir" && CGO_ENABLED=0 GOFLAGS=-buildvcs=false GOTOOLCHAIN=local go build ${GCFLAGS--gcflags=all=-l} -ldflags='-s -w' -work -x -o "$out/stock" . 2>&1)
work=$(echo "$log" | sed -n 's/^WORK=//p' | head -1)
link=$(echo "$log" | grep -m1 '/link -o ')
[ -n "$work" ] && [ -n "$link" ] || { echo "FAIL $dir: stock build failed"; echo "$log" | grep -v '^\(mkdir\|cat\|cd\|packagefile\|EOF\|#\)' | tail -20; exit 1; }
link=${link//\$WORK/$work}
cfg=$(echo "$link" | sed -n 's/.* -importcfg \([^ ]*\).*/\1/p')
id=$(echo "$link" | sed -n 's/.* -buildid=\([^ ]*\).*/\1/p')
main=${link##* }
echo "$work" > "$out/work"
echo "$hlink -o $out/new -importcfg $cfg -buildid=$id $main" > "$out/link.cmd"
if ! timeout "${TIMEOUT:-60}" prlimit --as=$((16 << 30)) "$hlink" ${HLINK_FLAGS:-} -o "$out/new" -importcfg "$cfg" -buildid="$id" "$main" > "$out/hlink.log" 2>&1; then
  echo "FAIL $(basename "$dir"): hlink failed; objects kept in $work"; head -20 "$out/hlink.log"; exit 1
fi
[ -s "$out/hlink.log" ] && cat "$out/hlink.log"
mask() { sed -E 's/0x[0-9a-f]+/0xX/g; s/goroutine [0-9]+/goroutine N/g'; }
(cd "$dir" && timeout "${TIMEOUT:-60}" "$out/stock" "$@" > "$out/stock.out" 2>&1; echo "exit $?" >> "$out/stock.out")
(cd "$dir" && timeout "${TIMEOUT:-60}" "$out/new" "$@" > "$out/new.out" 2>&1; echo "exit $?" >> "$out/new.out")
name="$(basename "$dir")${1:+ $*}"
if cmp -s <(mask < "$out/stock.out") <(mask < "$out/new.out"); then
  echo "PASS $name: same output ($(wc -l < "$out/new.out") lines, $(tail -1 "$out/new.out")); stock $(stat -c %s "$out/stock") bytes, hlink $(stat -c %s "$out/new") bytes"
  [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"
else
  echo "FAIL $name: output differs (stock | hlink); objects kept in $work"
  diff <(mask < "$out/stock.out") <(mask < "$out/new.out") | head -"${DIFF_LINES:-30}"
  exit 1
fi
