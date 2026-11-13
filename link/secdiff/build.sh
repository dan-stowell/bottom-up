#!/usr/bin/env bash
# Build the secdiff tools into build/ (of the repo):
#   build/linkmap     the stock linker (cmd/link of /usr/local/go) with stocklink.patch: -dumpmap FILE
#   build/fclinkmap   simd-lang's fclink with fclinkmap.patch: -dumpmap FILE (a stand-in for hlink)
#   build/secdiff     the symbolic diff (secdiff.go)
# All three are built inside the scratch GOROOT's cmd tree (they use cmd/internal and internal packages):
# cmd/linkmap, cmd/fclinkmap, cmd/secdiff. /usr/local/go and simd-lang are only read.
#
#   link/secdiff/build.sh [all|linkmap|fclinkmap|secdiff]
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
gr=${SB_GOROOT:-/home/exedev/targets/goroot-sb}
stock=${STOCK_GOROOT:-/usr/local/go}
fcsrc=${FCLINK_SRC:-/home/exedev/targets/simd-lang/driver/fclink}
mkdir -p "$repo/build"
gobuild() { (cd "$gr/src" && GOROOT="$gr" GOTOOLCHAIN=local GOFLAGS= "$gr/bin/go" build -o "$1" "$2"); }
what=${1:-all}

if [ "$what" = all ] || [ "$what" = linkmap ]; then
	d="$gr/src/cmd/linkmap"
	rm -rf "$d"
	cp -a "$stock/src/cmd/link" "$d"
	patch -s -p1 -d "$d" < "$here/stocklink.patch"
	# cmd/link/internal/... is importable only from cmd/link: point the copy at its own internal tree
	grep -rl '"cmd/link/internal/' "$d" --include='*.go' | xargs sed -i 's|"cmd/link/internal/|"cmd/linkmap/internal/|'
	gobuild "$repo/build/linkmap" cmd/linkmap
	echo "built build/linkmap"
fi
if [ "$what" = all ] || [ "$what" = fclinkmap ]; then
	d="$gr/src/cmd/fclinkmap"
	rm -rf "$d"
	mkdir -p "$d"
	cp "$fcsrc"/*.go "$d/"
	patch -s -p1 -d "$d" < "$here/fclinkmap.patch"
	gobuild "$repo/build/fclinkmap" cmd/fclinkmap
	echo "built build/fclinkmap"
fi
if [ "$what" = all ] || [ "$what" = secdiff ]; then
	d="$gr/src/cmd/secdiff"
	rm -rf "$d"
	mkdir -p "$d"
	cp "$here"/*.go "$d/"
	gobuild "$repo/build/secdiff" cmd/secdiff
	echo "built build/secdiff"
fi
