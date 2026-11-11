#!/usr/bin/env python3
"""stubmap.py -- what is the runtime stub made of? Instructions per part of build/stub.o, and which parts a
program's output depends on. (RUNTIME.md explains the split; tests run the pool with the "policy" parts
spoiled by noise to show that the output does not depend on them.)"""
import os, re, subprocess, collections
B = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build')
PARTS = [  # (part, depends-on-for-output?, labels that start a stretch of it)
 ('start: CPUs, heap, stacks, input', True, ['_start', 'useread', 'rd', 'nomem', 'rddone', 'inputdone', 'newstack']),
 ('output, exit, failure report', True, ['flush', 'wrange', 'fl1', 'fl2', 'wrfail', 'exit', 'fail', 'hex', 'softfail', 'justret']),
 ('threads: start, wake, wait', True, ['runall', 'ra1', 'ra2', 'ra3', 'startthreads', 'worker', 'wloop', 'wdone']),
 ('split a line into blocks and ranges', True, ['fork', 'fblock', 'fgood', 'fdone']),
 ('run one range from given carries', True, ['dojob', 'dj_real', 'dj_real1', 'dj_real0', 'dj_run', 'dj_ran1', 'dj_ran', 'dj_end', 'dj_dry1', 'dj_dry']),
 ('verify: every range started where the one before ended', True, ['fs7', 'fs8', 'fs9', 'fs95', 'fs96', 'fs10']),
 ('one-pass lines: scratch areas, copy into place', True, ['fg1', 'fg2', 'fg4', 'fc0', 'fc1', 'fc2', 'fc3', 'fc35', 'fc4', 'fcopied']),
 ('a block that failed: carries restored, run serially', True, ['fsbad']),
 ('guess: warm-up, states and prev carries of a range', False, ['dj_warm', 'dj_spec']),
 ('choose dry pass or one pass; derive carries; which ranges to rerun; retry', False, ['fscan', 'fs1', 'fs2', 'fs3', 'ff1', 'ff2', 'ff3', 'ff4', 'fs6', 'fsretry', 'derive']),
 ('chaos (tests)', False, ['chaos']),
]
def main():
    nm = subprocess.check_output(['nm', B + '/stub.o'], text=True)
    syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in nm.splitlines() if len(l.split()) == 3 and l.split()[1] in 'tT')
    dis = subprocess.check_output(['objdump', '-d', '-Mintel', B + '/stub.o'], text=True)
    addrs = [int(m.group(1), 16) for m in re.finditer(r'^\s+([0-9a-f]+):\t', dis, re.M)]
    end = dict(syms)  # name by address
    data0 = min(a for a, n in syms if n == 'nthreads')
    owner = {}
    for part, dep, labels in PARTS:
        for l in labels: owner[l] = part
    count = collections.Counter(); cur = None; names = {a: n for a, n in syms}; unknown = set()
    for a in addrs:
        if a >= data0: break
        if a in names:
            n = names[a]
            if n in owner: cur = owner[n]
            elif not re.fullmatch(r'\d+|L\d+', n): unknown.add(n)
        count[cur] += 1
    tot = sum(count.values())
    print('| part of the stub | instructions | can it change a program\'s output? |\n|---|---|---|')
    for part, dep, _ in PARTS: print(f'| {part} | {count[part]} | {"yes" if dep else "no: spoiled by `chaos`, output unchanged"} |')
    yes = sum(count[p] for p, d, _ in PARTS if d)
    print(f'| **total** | **{tot}** | {yes} yes, {tot - yes} no |')
    if unknown: print('unclassified labels (counted with the part before them):', ' '.join(sorted(unknown)))
main()
