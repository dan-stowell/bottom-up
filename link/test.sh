#!/usr/bin/env bash
# hlink's tests: build it, then
#   1. every program of link/progs through link/try.sh, without and with inlining (and the crashing runs);
#   2. identical output for -j 1, 2, 4 and 8 (a program of link/progs and the frozen MinIO link);
#   3. a short list of standard-library test binaries linked by hlink (link/stdtest.sh);
#   4. the frozen MinIO link: `--version` prints what the stock binary prints, and the live set (-dumplive)
#      equals fclink's for the same link;
#   5. output on hugetlbfs (skipped unless HUGETLB_DIR, default /dev/hugepages, is a writable hugetlbfs mount with
#      128 free huge pages): the programs run from there, -j 1/2/4/8 agree, MinIO's --version (also for the same
#      layout, -hugetlb, on an ordinary file system), and std tests (HSTD) run from hugetlbfs.
#   link/test.sh          (scratch output under ${HLINK_OUT:-/tmp/hlink-try})
# Environment: FCLINK (default: simd-lang's build/fclink), STD (the std list; default: sort strings strconv fmt reflect).
here=$(cd "$(dirname "$0")/.." && pwd)
out=${HLINK_OUT:-/tmp/hlink-try}; mkdir -p "$out"
export HLINK_OUT=$out
fclink=${FCLINK:-/home/exedev/targets/simd-lang/build/fclink}
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
if KEEP_WORK=1 GCFLAGS=" " "$here/link/try.sh" "$here/link/progs/ifaces" > /dev/null; then
  read -r -a cmd < "$out/ifaces/link.cmd"
  jcheck ifaces "${cmd[@]:3}"
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
if (cd "$minio" && timeout 120 prlimit --as=$((16 << 30)) "$hlink" -o "$out/frozen-minio/stock" -importcfg importcfg.link -dumplive "$out/frozen-minio/hlink.live" "${F[@]}" main.a); then
  if cmp -s <("$out/frozen-minio/stock" --version 2>&1) <("$minio/stock" --version 2>&1); then
    echo "PASS minio --version: $("$out/frozen-minio/stock" --version 2>&1 | head -1)"
  else
    echo "FAIL minio --version differs (stock | hlink)"; diff <("$minio/stock" --version 2>&1) <("$out/frozen-minio/stock" --version 2>&1) | head; fail=1
  fi
  if (cd "$minio" && timeout 120 "$fclink" -o "$out/frozen-minio/fclink" -importcfg importcfg.link -dumplive "$out/frozen-minio/fclink.live" "${F[@]}" main.a); then
    if cmp -s "$out/frozen-minio/hlink.live" "$out/frozen-minio/fclink.live"; then
      echo "PASS minio live set: $(wc -l < "$out/frozen-minio/hlink.live") symbols, equal to fclink's"
    else
      echo "FAIL minio live set differs from fclink's (fclink | hlink)"; diff "$out/frozen-minio/fclink.live" "$out/frozen-minio/hlink.live" | head; fail=1
    fi
    cmp -s "$out/frozen-minio/stock" "$out/frozen-minio/fclink" && echo "(minio: hlink's output is byte-identical to fclink's)"
  else
    echo "FAIL minio: fclink failed"; fail=1
  fi
else
  echo "FAIL minio: hlink failed"; fail=1
fi
rm -f "$out/frozen-minio/fclink"

# ---- 5. output on hugetlbfs (the image laid out on 2 MiB boundaries, bss in the file), and -hugetlb elsewhere
hdir=${HUGETLB_DIR:-/dev/hugepages}
hfree=$(awk '/HugePages_Free/{f=$2} /HugePages_Rsvd/{r=$2} END{print f-r}' /proc/meminfo)
if [ ! -w "$hdir" ] || [ "$(stat -f -c %T "$hdir" 2>/dev/null)" != hugetlbfs ] || [ "${hfree:-0}" -lt 128 ]; then
  echo "SKIP hugetlbfs: $hdir is not a writable hugetlbfs mount with at least 128 free huge pages ($hfree free; sysctl vm.nr_hugepages)"
else
  for p in hello gcstress traceback ifaces initorder; do
    for g in "-gcflags=all=-l" " "; do
      GCFLAGS=$g OUTDIR=$hdir run "$here/link/progs/$p"
    done
  done
  OUTDIR=$hdir run "$here/link/progs/gcstress" boom
  GCFLAGS=" " OUTDIR=$hdir run "$here/link/progs/traceback" crash
  HLINK_FLAGS=-hugetlb GCFLAGS=" " run "$here/link/progs/ifaces" # the same layout on an ordinary file system
  # the same output for every -j (one file on hugetlbfs at a time: each holds its pages)
  hj() { # name, then the link's arguments after -o
    local name=$1; shift; local ok=1 sums=() f=$hdir/hlink-test.$name.$$
    for j in 1 2 4 8; do
      if timeout 120 prlimit --as=$((16 << 30)) "$hlink" -j $j -o "$f" "$@" > /dev/null; then sums+=("$(md5sum < "$f")"); else ok=0; echo "FAIL hugetlbfs -j $j: $name: link failed"; fi
      rm -f "$f"
    done
    for s in "${sums[@]}"; do [ "$s" = "${sums[0]}" ] || { ok=0; echo "FAIL hugetlbfs -j: $name: outputs differ"; }; done
    [ $ok = 1 ] && echo "PASS hugetlbfs -j: $name: identical output for -j 1, 2, 4, 8"
    [ $ok = 1 ] || { fail=1; return 1; }
  }
  if KEEP_WORK=1 GCFLAGS=" " "$here/link/try.sh" "$here/link/progs/ifaces" > /dev/null; then
    read -r -a cmd < "$out/ifaces/link.cmd"
    hj ifaces "${cmd[@]:3}"
    rm -rf "$(cat "$out/ifaces/work")"
  else
    echo "FAIL hugetlbfs -j: ifaces did not build"; fail=1
  fi
  (cd "$minio" && hj minio -importcfg importcfg.link "${F[@]}" main.a) || fail=1
  # MinIO from hugetlbfs, and the same image with -hugetlb on an ordinary file system
  # (named stock, as the stock binary is: --version prints the program's name)
  mkdir -p "$hdir/hlink-test.$$" "$out/frozen-minio/hugetlb"
  for o in "$hdir/hlink-test.$$/stock" "$out/frozen-minio/hugetlb/stock"; do
    if (cd "$minio" && timeout 120 prlimit --as=$((16 << 30)) "$hlink" -hugetlb -o "$o" -importcfg importcfg.link "${F[@]}" main.a) &&
      cmp -s <("$o" --version 2>&1) <("$minio/stock" --version 2>&1); then
      echo "PASS minio --version from $(stat -f -c %T "$o"): $(stat -c %s "$o") bytes"
    else
      echo "FAIL minio -hugetlb to $o: link failed or --version differs"; fail=1
    fi
    rm -f "$o"
  done
  read -r -a hstd <<< "${HSTD:-sort fmt reflect runtime}"
  HLINK_HUGETLB=1 HUGETLB_DIR=$hdir "$here/link/stdtest.sh" "${hstd[@]}" || fail=1
  rm -rf "$hdir/hlink-test.$$" "$hdir"/hlink-test.*.$$ # (try.sh, toolexec.sh and stdtest.sh remove their own)
fi

[ $fail = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
