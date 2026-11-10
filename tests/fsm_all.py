#!/usr/bin/env python3
"""fsm_all.py COMPILER -- every one of the 256 functions on four states, as automaton transitions.
An automaton has 16 classes, so 16 tables hold all 256 functions (function f sends state s to
(f >> 2s) & 3). Each table runs over an input in which every ordered pair of its classes occurs at
every position of a 64-byte chunk and across chunk and thread-range boundaries; the compiled program
on one CPU, on all CPUs and in eager mode (which forks even small lines) must equal the scalar
interpreter. Suggested by the second audit: random tables mostly forget their start state, so
random testing says little about composition."""
import os, random, subprocess, sys
H = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(H, '..', 'boot'))
import strand
comp = sys.argv[1]; T = '/tmp/strand_fsm'; os.makedirs(T, exist_ok=True)
r = random.Random(3)
pairs = [(a, b) for a in range(16) for b in range(16)]
seq = bytearray()
for shift in range(64):                       # every pair at every chunk offset
    r.shuffle(pairs); seq += bytes(shift % 7 + 1)
    for a, b in pairs: seq += bytes((a, b))
seq += bytes(r.randrange(16) for _ in range(70001))
allowed = sorted(os.sched_getaffinity(0)); every = ','.join(map(str, allowed))
runs = 0
for k in range(16):
    table = ' '.join(str((f >> (2 * s)) & 3) for f in range(16 * k, 16 * k + 16) for s in range(4))
    src = f'{table} =d\nbytes /d dup prev 4 * + 48 + out\n'
    exe = f'{T}/p{k}'
    b = subprocess.run([comp], input=src.encode(), capture_output=True); assert b.returncode == 0, (k, b.returncode)
    open(exe, 'wb').write(b.stdout); os.chmod(exe, 0o755)
    want = strand.interp(src.encode(), bytes(seq))
    for cpus, mode in ((str(allowed[0]), []), (every, []), (every, ['eager'])):
        p = subprocess.run(['taskset', '-c', cpus, exe] + mode, input=bytes(seq), capture_output=True); runs += 1
        assert (p.returncode, p.stdout) == want, ('table', k, 'cpus', cpus, mode)
print('PASS', comp, '256 transition functions,', len(seq), 'bytes,', runs, 'runs')
