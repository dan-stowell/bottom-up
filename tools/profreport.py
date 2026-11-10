#!/usr/bin/env python3
"""profreport.py SRC.strand BINARY COUNTS [--lines N]
Turn prof.c output into: instruction mix (vector / mask / GPR bookkeeping / stub), per-phase and
per-line totals. Phases are the '# ----' banner comments in the source."""
import bisect, collections, os, re, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
from spec import *
srcp, binp, cntp = sys.argv[1:4]
src = open(srcp, 'rb').read()
out, cmap = strand.compile_(src, True)
T = strand.tables()
code0 = cmap[0][0]
dis = {}
for l in subprocess.check_output(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '--adjust-vma=0x400000',
                                  '--start-address=0x%x' % code0, binp], text=True).splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(\S+)', l)
    if m: dis[int(m.group(1), 16)] = m.group(2)
starts = [c[0] for c in cmap]
srclines = src.split(b'\n')
phase_of = {}; ph = 'tables'
for n, l in enumerate(srclines, 1):
    m = re.match(rb'# -+ (.*)', l)
    if m: ph = m.group(1).decode()[:60]
    phase_of[n] = ph
mix = collections.Counter(); byline = collections.Counter(); byphase = collections.Counter(); bytime = collections.Counter(); kinds = collections.Counter()
total = 0
for l in open(cntp):
    a, c = l.split(); a = int(a, 16); c = int(c); total += c
    if a < code0: mix['stub (syscall glue)'] += c; byphase['(startup/exit stub)'] += c; continue
    mn = dis.get(a, '?')
    k = bisect.bisect(starts, a) - 1
    line = cmap[k][1]
    byline[line] += c; byphase[phase_of[line]] += c
    if mn.startswith('vpgather'): mix['gather (16 serial loads each)'] += c; kinds['gather'] += c
    elif mn.startswith('vpscatter'): mix['scatter (16 serial stores each)'] += c; kinds['scatter'] += c
    elif mn.startswith('v'): mix['vector'] += c
    elif mn.startswith('k'): mix['mask register'] += c
    else: mix['GPR bookkeeping (loop control, pointers, checks, allocator)'] += c
print('total instructions executed: %d' % total)
print('\n| kind | instructions | share |\n|---|---|---|')
for k, v in mix.most_common(): print('| %s | %d | %.1f%% |' % (k, v, 100 * v / total))
print('\n| phase | instructions | share |\n|---|---|---|')
for k, v in byphase.most_common(): print('| %s | %d | %.1f%% |' % (k, v, 100 * v / total))
n = int(sys.argv[5]) if len(sys.argv) > 5 else 12
print('\n| line | instructions | source |\n|---|---|---|')
for line, v in byline.most_common(n): print('| %d | %d | `%s` |' % (line, v, srclines[line - 1][:70].decode('latin1')))
