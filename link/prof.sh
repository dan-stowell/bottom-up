#!/usr/bin/env bash
# Where do hlink's user-mode cycles go? Samples the instruction pointer with the CPU's cycle counter
# (tools/perfprof.c; kernel time is not in it: read that from -stats) and attributes samples to functions and
# to source lines.     link/prof.sh <workload> [hlink flags...]        TOP=40 lines, PERIOD=20000 cycles
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); w=$1; shift
d=${FROZEN:-/home/exedev/targets/frozen}/$w; out=${HLINK_OUT:-/tmp/hlink-try}/prof; mkdir -p "$out" "$here/build"
gcc -O2 -g -march=native -pthread -no-pie -fno-omit-frame-pointer -o "$here/build/hlink.prof" "$here/link/hlink.c"
gcc -O2 -o "$here/build/perfprof" "$here/tools/perfprof.c"
mapfile -t F < "$d/link.flags"
SAMPLE_OUT=$out/samples "$here/build/perfprof" "$here/build/hlink.prof" -o "$out/out" -importcfg "$d/importcfg.link" "${F[@]}" "$@" "$d/main.a"
python3 - "$here/build/hlink.prof" "$out/samples" "${TOP:-40}" <<'PY'
import subprocess, sys, collections, bisect
exe, samples, top = sys.argv[1], sys.argv[2], int(sys.argv[3])
syms = []
for line in subprocess.check_output(["nm", "-n", "--defined-only", exe], text=True).splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tT": syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
hist = {}
for line in open(samples):
    a, _, c = line.split(); hist[int(a)] = int(c)
total = sum(hist.values())
byfn = collections.Counter()
for a, c in hist.items():
    i = bisect.bisect_right(addrs, a) - 1
    byfn[syms[i][1] if i >= 0 else "?"] += c
print("%d samples\n-- by function (inlined callees are counted in their caller)" % total)
for f, c in byfn.most_common(top): print("%6.2f%%  %s" % (100.0 * c / total, f))
al = sorted(hist)
res = subprocess.run(["addr2line", "-e", exe, "-i", "-f"] + ["%#x" % a for a in al], capture_output=True, text=True).stdout
# addr2line -i prints several (function, file:line) pairs per address; take the innermost (first) one
lines = collections.Counter(); inner = collections.Counter()
out = subprocess.run(["addr2line", "-e", exe, "-f", "-a", "-i"] + ["%#x" % a for a in al], capture_output=True, text=True).stdout.splitlines()
cur = None; first = True
for l in out:
    if l.startswith("0x"): cur = int(l, 16); first = True; fn = None; continue
    if fn is None: fn = l; continue
    if first:
        loc = l.split("/")[-1].split(" ")[0]
        lines[loc] += hist[cur]; inner[fn] += hist[cur]; first = False
    fn = None
print("-- by innermost inlined function")
for f, c in inner.most_common(top): print("%6.2f%%  %s" % (100.0 * c / total, f))
print("-- by source line")
for f, c in lines.most_common(top): print("%6.2f%%  %s" % (100.0 * c / total, f))
PY
