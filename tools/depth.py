#!/usr/bin/env python3
"""depth.py FILE -- show where a line's stack depth goes wrong: for every line that is not a data line,
the depth after each token, and the first token at which it reaches 0 early, exceeds 8, or the line ends
at a depth other than 0. A writing aid; the compiler only says E_DEPTH."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
from spec import *
KWB = {k: i for i, k in enumerate(KEYWORDS)}
for ln, line in enumerate(open(sys.argv[1], encoding='latin1').read().split('\n'), 1):
    h = line.find('#'); toks = (line[:h] if h >= 0 else line).split()
    if not toks or toks[-1][0] == '=': continue
    byteline = toks[0] in ('bytes', 'bytesz') or toks[0][0] == "'"
    d = 0; trace = []
    for i, t in enumerate(toks):
        c0 = t[0]; sg = SIGILS.index(c0) + 1 if c0 in SIGILS and len(t) > 1 and opclass(min(ord(t[1]), 127)) in (1, 2, 3) else 0
        if not sg and t in KWB: cls = KWB[t]
        else: cls = CLSTAB[sg * 5 + opclass(min(ord(t[1 if sg else 0]), 127))]
        if i == 0: cls = LEADOF[cls]
        if byteline: cls = BYTEOF[cls]
        d += DELTA[cls]; trace.append(f'{t}({d})')
        if d > 8 or d < 0 or (d == 0 and i != len(toks) - 1):
            print(f'line {ln}: depth {d} at token {i} "{t}":  ...', ' '.join(trace[-14:])); break
    else:
        if d != 0: print(f'line {ln}: ends at depth {d}:  ...', ' '.join(trace[-10:]))
