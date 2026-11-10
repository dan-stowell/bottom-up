#!/usr/bin/env python3
"""liveset.py PROG.strand INPUT -- model the runtime allocator while interpreting PROG on INPUT:
how many blocks each size class ever needed (= fresh memory touched), and which line hit each peak."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
from spec import *
src = open(sys.argv[1], 'rb').read(); inp = open(sys.argv[2], 'rb').read()
toks, lines, nslots, pool = strand.analyze(src)
live = {}; free = {}; bumped = {}; pages = 0; peakline = {}; cur = {}
def cap(n): return 1 << ((4 * n + 127).bit_length())
def alloc(slot, n, line):
    global pages
    c = cap(n)
    if free.get(c): free[c] -= 1
    else:
        bumped[c] = bumped.get(c, 0) + 1; pages += (4 * n + 4159) // 4096; peakline[c] = line
    return c
orig_interp = strand.interp
# re-run the interpreter line by line, watching vector definitions
vecsz = {}
class Spy(dict):
    def __setitem__(self, k, v):
        global curline
        if k in LOCALS: return dict.__setitem__(self, k, v)      # chunk-local: lives in its slot
        if k in self and k in owned: free[owned[k]] = free.get(owned[k], 0) + 1
        owned[k] = alloc(k, len(v), curline)
        dict.__setitem__(self, k, v)
owned = {}; curline = 0
LOCALS = {t.slot for t in toks if t.cls == TEEL}
import builtins
# minimal re-implementation of the driver loop: reuse interp by patching dict creation is fragile, so inline it
def run():
    global curline
    vec = Spy(); 
    real_sorted = sorted
    # interp() builds its own dicts; easiest is to copy its body via exec with vec replaced
    import inspect
    body = inspect.getsource(strand.interp).replace('    vec = {}      # slot -> list', '    vec = SPYVEC')
    body = body.replace('            lt = lines[ln]\n', '            lt = lines[ln]\n            HOOK(ln, lt)\n')
    ns = dict(strand.__dict__); ns['SPYVEC'] = vec
    def hook(ln, lt):
        global curline
        # releases for the previous line happen before this one starts
        for t in hook.prev:
            if t.dying and t.slot in owned: free[owned[t.slot]] = free.get(owned[t.slot], 0) + 1; del owned[t.slot]
        hook.prev = lt; curline = ln
    hook.prev = []
    ns['HOOK'] = hook
    exec(body, ns)
    return ns['interp'](src, inp)
status, out = run()
srclines = src.split(b'\n')
print('exit', status, 'output bytes', len(out), ' modelled fresh pages:', pages)
print('| block capacity | blocks ever needed | last grew at line |\n|---|---|---|')
for c in sorted(bumped): print('| %d KiB | %d | %d: `%s` |' % (c // 1024, bumped[c], peakline[c], srclines[peakline[c] - 1][:60].decode('latin1')))
