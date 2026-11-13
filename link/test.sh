#!/usr/bin/env bash
# hlink's tests: build it, then
#   1. every program of link/progs through link/try.sh, without and with inlining (and the crashing runs);
#   2. identical output for -j 1, 2, 4 and 8 (a program of link/progs and the frozen MinIO link);
#   2b. overwriting an old output in place (the default) gives the bytes -fresh gives, over files of other sizes;
#      a symlink, a hard-linked file, a running binary and a directory are not written through; a failed link
#      leaves nothing; concurrent links of two programs to one path leave one of them whole and no temporaries;
#   2c. -teardown fork (the default), madvise and exit: the output runs as soon as the link returns, a failed
#      link's status and stderr are the same, -stats ends with the parent's row, a killed parent stops the child;
#      a link killed (SIGKILL, SIGTERM) at random moments leaves no output or the new one, never the old one; a
#      caller reading the linker's output until EOF returns even if the child stops right after completion;
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

# gone <output>: wait (up to 10 s) until no hlink with that -o runs (a detached child tears down after its parent)
gone() { for _ in $(seq 200); do pgrep -f -- "-o $1 " > /dev/null || return 0; sleep 0.05; done; return 1; }
# temps <dir>: how many link temporaries dir holds
temps() { find "$1" -maxdepth 1 -name '.*.hlink-*' | wc -l; }
rnd() { printf "0.%03d" $((RANDOM % ${1:-250})); } # a random sleep of up to $1 ms

# ---- 2b. overwriting the old output in place gives the bytes of a new file (-fresh), whatever was there
ow() {
  local d=$out/overwrite ok=1; rm -rf "$d"; mkdir -p "$d"; local f=$d/minio
  L() { (cd "$minio" && timeout 120 prlimit --as=$((16 << 30)) "$hlink" "$@" -importcfg importcfg.link "${F[@]}" main.a); }
  L -fresh -o "$d/fresh" || { echo "FAIL overwrite: -fresh link failed"; return 1; }
  for mb in 300 20 0; do # garbage, larger and smaller than the image
    head -c $((mb << 20)) /dev/zero | tr '\0' '\377' > "$f"; chmod 600 "$f"
    L -o "$f" || { ok=0; echo "FAIL overwrite: link over $mb MB failed"; }
    cmp -s "$d/fresh" "$f" || { ok=0; echo "FAIL overwrite: output over a $mb MB file differs from -fresh"; }
    [ "$(stat -c %a "$f")" = "$(stat -c %a "$d/fresh")" ] || { ok=0; echo "FAIL overwrite: mode $(stat -c %a "$f") after overwriting"; }
  done
  "$f" --version > /dev/null || { ok=0; echo "FAIL overwrite: the overwritten binary does not run"; }
  # not reused: a symlink (replaced by a file, its target untouched), a file with another hard link
  head -c 1000 /dev/zero > "$d/target"; ln -s "$d/target" "$d/sym"; ln -f "$f" "$d/hard"
  L -X main.x=1 -o "$d/sym" && L -X main.x=1 -o "$f" || ok=0
  [ ! -L "$d/sym" ] && [ "$(stat -c %s "$d/target")" = 1000 ] && cmp -s "$d/fresh" "$d/hard" || { ok=0; echo "FAIL overwrite: wrote through a symlink or a hard link"; }
  # ... nor a binary that is running (it keeps its old inode; the new output is a new file)
  cp /bin/sleep "$f"; "$f" 60 & local pid=$!; sleep 0.2
  L -o "$f" || { ok=0; echo "FAIL overwrite: link over a running binary failed"; }
  kill $pid 2> /dev/null; wait $pid 2> /dev/null
  cmp -s "$d/fresh" "$f" || { ok=0; echo "FAIL overwrite: link over a running binary gave other bytes"; }
  # a failed link leaves no output
  L -X nodot -o "$f" > /dev/null 2>&1
  [ -e "$f" ] && { ok=0; echo "FAIL overwrite: a failed link left the old output"; }
  # a directory at the output path is not replaced
  mkdir "$d/dir"; L -o "$d/dir" > /dev/null 2>&1 && { ok=0; echo "FAIL overwrite: linked over a directory"; }
  [ -d "$d/dir" ] || { ok=0; echo "FAIL overwrite: the directory at the output path is gone"; }
  # two links of different programs (MinIO, MinIO -nodce: 109 and 172 MB) to one path, started together (one of
  # them, at random, after a random delay, so either may claim the old file and either may finish last), over what
  # the previous round left: both succeed, the path holds one of the two whole, and no temporaries remain
  L -nodce -fresh -o "$d/nodce" || ok=0
  local won1=0 won2=0 rounds=${ROUNDS:-50} p1 p2 s1 s2
  for _ in $(seq "$rounds"); do
    if [ $((RANDOM % 2)) = 0 ]; then s1=$(rnd 250) s2=0; else s1=0 s2=$(rnd 150); fi
    (sleep "$s1"; L -o "$f") & p1=$!
    (sleep "$s2"; L -nodce -o "$f") & p2=$!
    wait $p1 || { ok=0; echo "FAIL overwrite: concurrent link of MinIO failed ($?)"; }
    wait $p2 || { ok=0; echo "FAIL overwrite: concurrent link of MinIO -nodce failed ($?)"; }
    gone "$f"
    if cmp -s "$f" "$d/fresh"; then won1=$((won1 + 1)); elif cmp -s "$f" "$d/nodce"; then won2=$((won2 + 1)); else ok=0; echo "FAIL overwrite: concurrent links left neither output whole"; fi
  done
  [ "$(temps "$d")" = 0 ] || { ok=0; echo "FAIL overwrite: concurrent links left temporaries"; ls -a "$d"; }
  echo "(overwrite: $rounds concurrent rounds: MinIO won $won1, MinIO -nodce won $won2)"
  rm -rf "$d"
  [ $ok = 1 ] && echo "PASS overwrite: in place over 300, 20 and 0 MB of garbage = -fresh; symlink, hard link, running binary, directory, failed link; $rounds concurrent rounds whole"
  [ $ok = 1 ]
}
ow || fail=1

# ---- 2c. the end of the process (-teardown fork, the default; madvise; exit = -nodetach)
td() {
  local d=$out/teardown ok=1; rm -rf "$d"; mkdir -p "$d"
  L() { (cd "$minio" && timeout 120 prlimit --as=$((16 << 30)) "$hlink" "$@" -importcfg importcfg.link "${F[@]}" main.a); }
  for t in fork madvise exit; do
    # the output runs as soon as the linker returns (a detached child that still mapped it: ETXTBSY)
    for r in 1 2; do
      L -teardown=$t -o "$d/stock" > /dev/null && "$d/stock" --version > /dev/null || { ok=0; echo "FAIL teardown $t: the output did not run right after the link"; }
    done
    # a failed link: the same status and stderr whichever way it ends
    L -teardown=$t -X nodot -o "$d/bad" > /dev/null 2> "$d/err.$t"; echo "exit $?" >> "$d/err.$t"
    [ -e "$d/bad" ] && { ok=0; echo "FAIL teardown $t: a failed link left an output"; }
  done
  cmp -s "$d/err.fork" "$d/err.exit" && cmp -s "$d/err.madvise" "$d/err.exit" && grep -q "^exit 2$" "$d/err.exit" ||
    { ok=0; echo "FAIL teardown: a failed link's status or stderr differs between fork, madvise and exit"; cat "$d"/err.*; }
  # -stats: the child's table, then the parent's row for the moment the output was complete
  L -stats -o "$d/stock" > "$d/stats" && tail -1 "$d/stats" | grep -q "^complete .*as the parent sees it" ||
    { ok=0; echo "FAIL teardown fork: -stats does not end with the parent's complete row"; tail -3 "$d/stats"; }
  HLINK_NODETACH=1 L -stats -o "$d/stock" | grep -q "^complete .*one process" || { ok=0; echo "FAIL teardown: HLINK_NODETACH=1 did not keep one process"; }
  cmp -s <("$d/stock" --version 2>&1) <("$minio/stock" --version 2>&1) || { ok=0; echo "FAIL teardown: --version differs"; }
  # the parent killed: the child stops too (it does not go on and write the output)
  (cd "$minio" && exec "$hlink" -fresh -o "$d/killed" -importcfg importcfg.link "${F[@]}" main.a) & local pid=$!
  sleep 0.05; kill -9 $pid; wait $pid 2> /dev/null; sleep 0.5
  pgrep -f -- "-o $d/killed" > /dev/null && { ok=0; echo "FAIL teardown: the child outlived its killed parent"; pkill -9 -f -- "-o $d/killed"; }
  [ -e "$d/killed" ] && { ok=0; echo "FAIL teardown: a killed link left an output"; }
  # killed at random moments of a link over an old executable (MinIO -nodce; the new one is MinIO), with and
  # without the fork, by SIGKILL (to the parent: the child dies with it) and SIGTERM: afterwards the path is
  # absent or the complete new image, never the old one or a part; SIGTERM leaves no temporary
  L -nodce -fresh -o "$d/old" > /dev/null && L -fresh -o "$d/new" > /dev/null || ok=0
  local f=$d/k absent=0 new=0 n=0
  for sig in TERM KILL; do # (TERM first: what KILL leaves behind stays until the sweep below)
    for t in fork exit; do
      for _ in $(seq ${KILLROUNDS:-20}); do
        cp "$d/old" "$f"
        (cd "$minio" && exec "$hlink" -teardown=$t -o "$f" -importcfg importcfg.link "${F[@]}" main.a > /dev/null) & pid=$!
        sleep "$(rnd 300)"; kill -$sig $pid 2> /dev/null; wait $pid 2> /dev/null
        gone "$f" || { ok=0; echo "FAIL teardown: a killed link's child is still running"; pkill -9 -f -- "-o $f "; }
        n=$((n + 1))
        if [ ! -e "$f" ]; then absent=$((absent + 1)); elif cmp -s "$f" "$d/new"; then new=$((new + 1)); else
          ok=0; cmp -s "$f" "$d/old" && echo "FAIL teardown: SIG$sig ($t) left the old output" || echo "FAIL teardown: SIG$sig ($t) left a partial output"
        fi
        [ $sig = TERM ] && [ "$(temps "$d")" != 0 ] && { ok=0; echo "FAIL teardown: SIGTERM ($t) left a temporary"; ls -a "$d"; find "$d" -maxdepth 1 -name '.*.hlink-*' -delete; }
      done
    done
  done
  # what SIGKILL left behind goes with the next link that finds no output there
  local left; left=$(temps "$d"); rm -f "$f"
  L -o "$f" > /dev/null && gone "$f" && [ "$(temps "$d")" = 0 ] || { ok=0; echo "FAIL teardown: the next link did not remove dead links' temporaries"; ls -a "$d"; }
  echo "(teardown: $n killed links: $absent left no output, $new the new one; SIGKILL left $left temporaries, removed by the next link)"
  # a caller that reads stdout and stderr through a pipe until EOF returns as soon as the output is complete, even
  # when the child stops right after saying so (HLINK_DEBUG_STOP_AFTER_DONE=1 stops it with SIGSTOP there)
  (cd "$minio" && HLINK_DEBUG_STOP_AFTER_DONE=1 "$hlink" -stats -o "$d/stopped" -importcfg importcfg.link "${F[@]}" main.a 2>&1 | cat > "$d/piped") & pid=$!
  local w=0; while kill -0 $pid 2> /dev/null && [ $w -lt 200 ]; do sleep 0.05; w=$((w + 1)); done
  kill -0 $pid 2> /dev/null && { ok=0; echo "FAIL teardown: a caller reading the linker's output did not see EOF while the child was stopped"; }
  local child; child=$(pgrep -f -- "-o $d/stopped ")
  [ -n "$child" ] && [[ "$(ps -o stat= -p "$child")" == T* ]] || { ok=0; echo "FAIL teardown: HLINK_DEBUG_STOP_AFTER_DONE did not stop the child"; }
  pkill -9 -f -- "-o $d/stopped "; wait $pid
  grep -q "^complete .*as the parent sees it" "$d/piped" && "$d/stopped" --version > /dev/null ||
    { ok=0; echo "FAIL teardown: a stopped child's link is not complete"; tail -2 "$d/piped"; }
  rm -rf "$d"
  [ $ok = 1 ] && echo "PASS teardown: fork, madvise and exit: outputs run at once, failures alike, -stats complete row, HLINK_NODETACH, killed parent; $n links killed at random never left the old output; EOF with a stopped child"
  [ $ok = 1 ]
}
td || fail=1

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
    # The same number of live symbols as fclink. The names can differ where copies of a content-addressed
    # symbol carry different names (X.arginfo1 and Y.arginfo1 with the same bytes): fclink keeps the first of
    # all copies, hlink the first of the reached ones. Whether the right symbols are live is secdiff's job.
    nh=$(wc -l < "$out/frozen-minio/hlink.live"); nf=$(wc -l < "$out/frozen-minio/fclink.live")
    if [ "$nh" = "$nf" ]; then
      echo "PASS minio live set: $nh symbols, as many as fclink's ($(diff <(LC_ALL=C sort "$out/frozen-minio/fclink.live") <(LC_ALL=C sort "$out/frozen-minio/hlink.live") | grep -c '^>') names differ)"
    else
      echo "FAIL minio live set: $nh symbols, fclink has $nf"; fail=1
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
  # overwriting on hugetlbfs: the same claim and final rename; the old file is unlinked at the claim, and its huge
  # pages are free again once the links are gone
  o=$hdir/hlink-test.$$/stock; f0=$(awk '/HugePages_Free/{print $2}' /proc/meminfo); hok=1
  for _ in 1 2 3; do
    (cd "$minio" && timeout 120 prlimit --as=$((16 << 30)) "$hlink" -o "$o" -importcfg importcfg.link "${F[@]}" main.a) &&
      cmp -s <("$o" --version 2>&1) <("$minio/stock" --version 2>&1) || { hok=0; echo "FAIL hugetlbfs overwrite: link over the old output failed or --version differs"; }
  done
  gone "$o"; [ "$(temps "$hdir/hlink-test.$$")" = 0 ] || { hok=0; echo "FAIL hugetlbfs overwrite: temporaries left"; }
  rm -f "$o"; f1=$(awk '/HugePages_Free/{print $2}' /proc/meminfo)
  [ "$f1" -ge "$f0" ] || { hok=0; echo "FAIL hugetlbfs overwrite: $f0 huge pages free before, $f1 after"; }
  [ $hok = 1 ] && echo "PASS hugetlbfs overwrite: 3 links over the old output, no temporaries, huge pages freed ($f0 free before, $f1 after)" || fail=1
  read -r -a hstd <<< "${HSTD:-sort fmt reflect runtime}"
  HLINK_HUGETLB=1 HUGETLB_DIR=$hdir "$here/link/stdtest.sh" "${hstd[@]}" || fail=1
  rm -rf "$hdir/hlink-test.$$" "$hdir"/hlink-test.*.$$ # (try.sh, toolexec.sh and stdtest.sh remove their own)
fi

[ $fail = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
