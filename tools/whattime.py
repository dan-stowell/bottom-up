#!/usr/bin/env python3
"""whattime.py SOURCE SAMPLES -- attribute the samples of tools/sample.c to the tokens of the Strand
program SOURCE: share of time by line and by kind of token. An address belongs to the token whose
body starts last at or before it."""
import bisect, collections, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
src = open(sys.argv[1], 'rb').read()
_, cmap = strand.compile_(src, want_map=True); addrs = [a for a, _, _ in cmap]
OPS = set(strand.KEYWORDS)
line = collections.Counter(); kinds = collections.Counter(); tot = 0
for l in open(sys.argv[2]):
    a, _, c = map(int, l.split()); i = bisect.bisect_right(addrs, a) - 1
    if i < 0: continue
    _, ln, raw = cmap[i]
    k = raw if raw in OPS else raw[0] + 'x' if raw[0] in "@/\\%^:'<>.[{!}?$,()&~" and len(raw) > 1 else 'literal' if raw[0].isdigit() or raw[0].isupper() else 'push'
    line[ln] += c; kinds[k] += c; tot += c
lines = src.split(b'\n')
print(tot, 'samples in generated code')
for ln, c in line.most_common(6): print(f'{100 * c / tot:5.1f}%  line {ln}: {lines[ln - 1][:70].decode("latin1")}')
print()
for k, c in kinds.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 16): print(f'{100 * c / tot:5.1f}%  {k}')
