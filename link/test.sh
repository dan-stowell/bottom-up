#!/usr/bin/env bash
# hlink's tests: build it, then
#   1. every program of link/progs through link/try.sh, without and with inlining (and the crashing runs);
#   2. identical output for -j 1, 2, 4 and 8 (a program of link/progs and the frozen MinIO link);
#   3. a short list of standard-library test binaries linked by hlink (link/stdtest.sh);
#   4. the frozen MinIO link: `--version` prints what the stock binary prints;
#   5. the libraries are loaded (so objects numbered) in the stock linker's order, which decides which copy of a
#      duplicated symbol is kept (`-v=2` of both linkers, for ifaces and MinIO);
#   6. secdiff (link/secdiff.sh): hello and ifaces with and without inlining, and MinIO, have no differences
#      from the stock linker's output beyond secdiff's rules (it compares symbol sets, which fclink's live
#      set was compared with before: fclink keeps other copies of duplicated symbols).
#   link/test.sh          (scratch output under ${HLINK_OUT:-/tmp/hlink-try})
# Environment: STD (the std list; default: sort strings strconv fmt reflect crypto/fips140).
here=$(cd "$(dirname "$0")/.." && pwd)
out=${HLINK_OUT:-/tmp/hlink-try}; mkdir -p "$out"
export HLINK_OUT=$out
minio=${MINIO:-/home/exedev/targets/frozen/minio}
hlink=$here/build/hlink
fail=0
"$here/link/build.sh" || exit 1

# ---- 1. the programs
run() { "$here/link/try.sh" "$@" || fail=1; }
for p in hello gcstress traceback ifaces initorder; do
  for g in "-gcflags=all=-l" " "; do
    GCFLAGS=$g run "$here/link/progs/$p"
  done
done
run "$here/link/progs/gcstress" boom # a crash: exit status 2 and the same traceback text
run "$here/link/progs/traceback" crash
GCFLAGS=" " run "$here/link/progs/traceback" crash

# ---- 2. the same output for every -j
jcheck() { # name, then the link's arguments after -o
  local name=$1; shift; local ok=1
  for j in 1 2 4 8; do
    timeout 120 prlimit --as=$((16 << 30)) "$hlink" -j $j -o "$out/$name.j$j" "$@" > /dev/null || { ok=0; echo "FAIL -j $j: $name: link failed"; }
  done
  for j in 2 4 8; do
    cmp -s "$out/$name.j1" "$out/$name.j$j" || { ok=0; echo "FAIL -j: $name: -j $j differs from -j 1"; }
  done
  [ $ok = 1 ] && echo "PASS -j: $name: identical output for -j 1, 2, 4, 8 ($(stat -c %s "$out/$name.j1") bytes)"
  rm -f "$out/$name".j[1248]
  [ $ok = 1 ] || { fail=1; return 1; }
}
# (5.) the load order: the stock linker's `-v=2` prints "ldobj: FILE (PKG)" per library as it loads it; so does hlink
[ -x "$here/build/linkmap" ] || "$here/link/secdiff/build.sh" linkmap > /dev/null
ordercheck() { # name, then the link's arguments after -o
  local name=$1; shift
  timeout 300 "$here/build/linkmap" -v=2 -s -w -o "$out/$name.order" "$@" > "$out/$name.stock.v" 2>&1 || { echo "FAIL order: $name: the stock link failed"; fail=1; return 1; }
  timeout 120 prlimit --as=$((16 << 30)) "$hlink" -v=2 -o "$out/$name.order" "$@" > "$out/$name.hlink.v" || { echo "FAIL order: $name: link failed"; fail=1; return 1; }
  rm -f "$out/$name.order"
  if [ -s "$out/$name.stock.v" ] && cmp -s <(grep '^ldobj: ' "$out/$name.stock.v") <(grep '^ldobj: ' "$out/$name.hlink.v"); then
    echo "PASS order: $name: $(grep -c '^ldobj: ' "$out/$name.hlink.v") libraries loaded in the stock linker's order"
  else
    echo "FAIL order: $name: load order differs from the stock linker's (stock | hlink)"
    diff <(grep '^ldobj: ' "$out/$name.stock.v") <(grep '^ldobj: ' "$out/$name.hlink.v") | head -5; fail=1; return 1
  fi
}
if KEEP_WORK=1 GCFLAGS=" " "$here/link/try.sh" "$here/link/progs/ifaces" > /dev/null; then
  read -r -a cmd < "$out/ifaces/link.cmd"
  jcheck ifaces "${cmd[@]:3}"
  ordercheck ifaces "${cmd[@]:3}"
  rm -rf "$(cat "$out/ifaces/work")"
else
  echo "FAIL -j: ifaces did not build"; fail=1
fi
mapfile -t F < "$minio/link.flags"
(cd "$minio" && jcheck minio -importcfg importcfg.link "${F[@]}" main.a) || fail=1

# ---- 3. standard-library tests
read -r -a std <<< "${STD:-sort strings strconv fmt reflect crypto/fips140}"
"$here/link/stdtest.sh" "${std[@]}" || fail=1

# ---- 4. MinIO
mkdir -p "$out/frozen-minio"
if (cd "$minio" && timeout 120 prlimit --as=$((16 << 30)) "$hlink" -o "$out/frozen-minio/stock" -importcfg importcfg.link "${F[@]}" main.a); then
  if cmp -s <("$out/frozen-minio/stock" --version 2>&1) <("$minio/stock" --version 2>&1); then
    echo "PASS minio --version: $("$out/frozen-minio/stock" --version 2>&1 | head -1)"
  else
    echo "FAIL minio --version differs (stock | hlink)"; diff <("$minio/stock" --version 2>&1) <("$out/frozen-minio/stock" --version 2>&1) | head; fail=1
  fi
else
  echo "FAIL minio: hlink failed"; fail=1
fi
rm -f "$out/frozen-minio/stock"

# ---- 5. the load order of MinIO (ifaces: above)
(cd "$minio" && ordercheck minio -importcfg importcfg.link "${F[@]}" main.a) || fail=1

# ---- 6. secdiff against the stock linker's output
sdcheck() { # name, GCFLAGS, dir
  local log=$out/secdiff.$1.log
  if GCFLAGS=$2 "$here/link/secdiff.sh" "$3" > "$log" 2>&1; then
    echo "PASS secdiff: $1: $(awk '$1 == "symbols" && NF == 6 {print $2 " symbols matched, " $5 " unmatched (all by rule)"}' "$log")"
  else
    echo "FAIL secdiff: $1 ($log):"; sed -n '/^Differences/,$p' "$log" | head -20; fail=1
  fi
}
for p in hello ifaces; do
  sdcheck "$p" " " "$here/link/progs/$p"
  sdcheck "$p-l" "-gcflags=all=-l" "$here/link/progs/$p"
done
sdcheck minio " " "$minio"

[ $fail = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
