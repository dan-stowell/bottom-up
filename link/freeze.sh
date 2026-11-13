#!/usr/bin/env bash
# Freeze the stock objects of a Go main package: build it with the stock toolchain (-a -work), then copy every
# object the link reads into one directory with an importcfg.link that points there, and record the link line.
#   link/freeze.sh <dir of main package> <name> [extra go build flags...]   ->  /home/exedev/targets/frozen/<name>/
# FREEZE_GOFLAGS replaces the default GOFLAGS=-mod=mod (set it empty for a workspace or vendored module).
# The directory then holds: objs/NNNN.a, importcfg.link, main.a, link.flags (the stock link's flags, one per
# line, without -o/-importcfg/the object), stock (the stock linker's binary, with DWARF), build.log.
set -euo pipefail
src=$(cd "$1" && pwd); name=$2; shift 2
dst=${FROZEN:-/home/exedev/targets/frozen}/$name
rm -rf "$dst/objs" "$dst/main.a"; mkdir -p "$dst/objs"
( cd "$src" && GOTOOLCHAIN=local GOFLAGS=${FREEZE_GOFLAGS--mod=mod} CGO_ENABLED=0 go build -a -work -x -trimpath "$@" -o "$dst/stock" . ) > "$dst/build.log" 2>&1
work=$(sed -n 's/^WORK=//p' "$dst/build.log" | head -1)
line=$(grep '/link -o ' "$dst/build.log" | tail -1); line=${line//\$WORK/$work}
cfg=$(sed -n 's/.* -importcfg \([^ ]*\).*/\1/p' <<<"$line"); main=${line##* }
n=0; : > "$dst/importcfg.link"
while IFS= read -r l; do
  case $l in
    packagefile\ *) p=${l#packagefile }; f=${p#*=}; p=${p%%=*}
      if [ "$f" = "$main" ]; then t=$dst/main.a; else n=$((n+1)); t=$(printf '%s/objs/%04d.a' "$dst" $n); fi
      cp "$f" "$t"; echo "packagefile $p=$t" >> "$dst/importcfg.link";;
    *) echo "$l" >> "$dst/importcfg.link";;
  esac
done < "$cfg"
[ -f "$dst/main.a" ] || cp "$main" "$dst/main.a"
# the flags between the importcfg and the object, one per line (the go command quotes them as a shell would)
python3 -c 'import shlex, sys
a = shlex.split(sys.argv[1]); i = a.index("-importcfg")
print("\n".join(x for x in a[i + 2:-1] if not x.startswith("-extld=")))' "$line" > "$dst/link.flags"
rm -rf "$work"
echo "$name: $(ls "$dst/objs" | wc -l) objects + main.a, $(du -sb "$dst/objs" "$dst/main.a" | awk '{s+=$1} END {printf "%.1f MB", s/1e6}') in $dst"
