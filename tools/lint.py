#!/usr/bin/env python3
"""lint.py PROG.strand -- say *where* a compile error is (the compilers only give a number).
Re-derives depth/stencil/carry problems per line with the reference's tables."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
from spec import *
src = open(sys.argv[1], 'rb').read()
T = strand.tables()
kw = {k.encode(): i for i, k in enumerate(KEYWORDS)}
for n, line in enumerate(src.split(b'\n'), 1):
    h = line.find(b'#')
    if h >= 0: line = line[:h]
    toks = line.split()
    if not toks or toks[-1][:1] == b'=': continue
    d = 0; seen = set()
    for k, t in enumerate(toks):
        c0 = t[0]; c1 = t[1] if len(t) > 1 else 32
        sg = sigil(c0) if len(t) > 1 and opclass(min(c1, 127)) in (1, 2, 3) else 0
        cls = kw[t] if sg == 0 and t in kw else CLSTAB[sg * 5 + opclass(min(c1 if sg else c0, 127))]
        if k == 0: cls = LEADOF[cls]
        if cls >= NCODE: print('line %d: token %d %r is not code (class %d)' % (n, k, t, cls)); break
        if not 0 <= d <= 8 or T['slen'][cls * ND + d] == 0: print('line %d: token %d %r not valid at depth %d' % (n, k, t, d)); break
        if CARRY[cls]:
            if (CARRY[cls], d) in seen: print('line %d: token %d %r shares a carry at depth %d' % (n, k, t, d)); break
            seen.add((CARRY[cls], d))
        d += DELTA[cls]
        if (d == 0) != (k == len(toks) - 1): print('line %d: depth %d after token %d %r' % (n, d, k, t)); break
