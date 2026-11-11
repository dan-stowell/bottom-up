#!/usr/bin/env python3
"""whereis.py SOURCE HIST -- attribute the instructions counted by simdratio (SIMDRATIO_HIST=HIST) to the
tokens of the Strand program SOURCE that was running. An address belongs to the token whose body starts
last at or before it, so a line's prologues and releases are charged to the line before it or to its own
last token. Prints the scalar instructions by kind of token and by line."""
import bisect, collections, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
src = open(sys.argv[1], 'rb').read()
_, cmap = strand.compile_(src, want_map=True)
addrs = [a for a, _, _ in cmap]
K = ['vector', 'gather', 'mask', 'scalar']
bykind = collections.defaultdict(lambda: [0] * 4); byline = collections.defaultdict(lambda: [0] * 4); tot = [0] * 4
def kind(raw):
    if raw[0] in "@[{" and len(raw) > 1: return 'gather ' + raw[0] + 'x'
    if raw[0] in "!}" and len(raw) > 1: return 'scatter ' + raw[0] + 'x'
    if raw[0] in ">:%?.,'<()$^~&/" and len(raw) > 1: return raw[0] + 'x'
    if raw[0].isdigit() or raw[0].isupper(): return 'scalar operand'
    if raw[0].islower() and raw not in strand.KEYWORDS: return 'vector push'
    return raw
for l in open(sys.argv[2]):
    a, k, c = map(int, l.split()); i = bisect.bisect_right(addrs, a) - 1
    if i < 0: continue
    _, line, raw = cmap[i]
    bykind[kind(raw)][k] += c; byline[line][k] += c; tot[k] += c
print(f'{"scalar":>10} {"vector":>10} {"gather":>9} {"mask":>9}  by kind of token')
for kd, v in sorted(bykind.items(), key=lambda kv: -kv[1][3])[:22]: print(f'{v[3]:10d} {v[0]:10d} {v[1]:9d} {v[2]:9d}  {kd}')
print(f'{tot[3]:10d} {tot[0]:10d} {tot[1]:9d} {tot[2]:9d}  total')
lines = src.split(b'\n')
print(f'\n{"scalar":>10} {"vector":>10}  by line (top 12)')
for ln, v in sorted(byline.items(), key=lambda kv: -kv[1][3])[:12]: print(f'{v[3]:10d} {v[0]:10d}  {lines[ln - 1][:110].decode("latin1")}')
