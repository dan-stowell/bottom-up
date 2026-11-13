#!/usr/bin/env bash
# The section-by-section oracle: link the same objects with the stock linker (patched only to write a map,
# build/linkmap, output byte-identical to `go tool link`) and with the linker under test, then compare the
# two binaries symbol by symbol with build/secdiff (link/secdiff/).
#
#   link/secdiff.sh <frozen workload dir | main package dir>
#
# A frozen workload dir has importcfg.link, main.a and link.flags (one flag per line). A main package dir is
# built first with `go build -a -work -x` (stock toolchain) to get the objects and the link line.
# Environment: LINKER (default build/hlink; build/fclinkmap works too: it must accept -dumpmap FILE),
# HLINK_OUT (default /tmp/hlink-try; scratch goes in $HLINK_OUT/secdiff/<name>), SECDIFF_FLAGS (e.g. "-n 20 -v"),
# GCFLAGS (main package dir only: extra go build flags, e.g. -gcflags=all=-l), SECDIFF_AS (address-space
# limit for secdiff, default 20G).
set -e
repo=$(cd "$(dirname "$0")/.." && pwd)
linker=${LINKER:-$repo/build/hlink}
case $linker in /*) ;; *) linker=$repo/$linker ;; esac
dir=$(cd "${1:?usage: link/secdiff.sh <frozen workload dir | main package dir>}" && pwd)
name=$(basename "$dir")
out=${HLINK_OUT:-/tmp/hlink-try}/secdiff/$name
mkdir -p "$out"
for t in linkmap secdiff; do
	[ -x "$repo/build/$t" ] || "$repo/link/secdiff/build.sh" "$t" >&2
done
[ -x "$linker" ] || { echo "secdiff.sh: no linker $linker (set LINKER)" >&2; exit 2; }
export GOROOT=${STOCK_GOROOT:-/usr/local/go} # (what `go build` gives the linker: runtime.defaultGOROOT, file names)

flags=()
if [ -f "$dir/importcfg.link" ] && [ -f "$dir/main.a" ]; then
	cfg=$dir/importcfg.link
	main=$dir/main.a
	while IFS= read -r f; do [ -n "$f" ] && flags+=("$f"); done < "$dir/link.flags"
else
	log=$(cd "$dir" && CGO_ENABLED=0 GOFLAGS=-buildvcs=false GOTOOLCHAIN=local "$GOROOT/bin/go" build -a ${GCFLAGS:-} -ldflags='-s -w' -work -x -o "$out/gobuild" . 2>&1) ||
		{ echo "$log" | tail -20; exit 1; }
	work=$(echo "$log" | sed -n 's/^WORK=//p' | head -1)
	link=$(echo "$log" | grep -m1 '/link -o ')
	[ -n "$work" ] && [ -n "$link" ] || { echo "secdiff.sh: no link line in the build log" >&2; exit 1; }
	link=${link//\$WORK/$work}
	read -r -a words <<< "${link#*/link }"
	main=${words[${#words[@]}-1]}
	for ((i = 0; i < ${#words[@]} - 1; i++)); do
		case ${words[i]} in
		-o | -importcfg) i=$((i + 1)); [ "${words[i-1]}" = -importcfg ] && cfg=${words[i]} ;;
		-s | -w | -extld=*) ;;
		*) flags+=("${words[i]}") ;;
		esac
	done
	cp "$cfg" "$out/importcfg.link"
	echo "$work" > "$out/work"
fi
printf '%s\n' "${flags[@]}" > "$out/link.flags"

t0=$(date +%s.%N)
"$repo/build/linkmap" -o "$out/stock" -s -w "${flags[@]}" -dumpmap "$out/stock.map" -importcfg "$cfg" "$main"
t1=$(date +%s.%N)
"$linker" -o "$out/hlink" "${flags[@]}" -dumpmap "$out/hlink.map" -importcfg "$cfg" "$main"
t2=$(date +%s.%N)
echo "linked $name: stock $(stat -c %s "$out/stock") bytes ($(echo "$t1 - $t0" | bc) s), $(basename "$linker") $(stat -c %s "$out/hlink") bytes ($(echo "$t2 - $t1" | bc) s); maps in $out"
[ -n "${work:-}" ] && [ -z "${KEEP_WORK:-}" ] && rm -rf "$work"
exec prlimit --as=$(numfmt --from=iec "${SECDIFF_AS:-20G}") "$repo/build/secdiff" ${SECDIFF_FLAGS:-} "$out/hlink.map" "$out/hlink" "$out/stock.map" "$out/stock"
