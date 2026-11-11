#!/usr/bin/env python3
"""primcost.py [INPUT] -- what does each byte-line primitive cost? Cycles and instructions per 64-byte chunk.

Each primitive is repeated on one line (a chain: N lookups, N compares, ...) over INPUT (default: 64 MB of
build/gosmall.go repeated), run on one CPU under build/perfprof, and the cost of the bare line `bytes $S` is
subtracted. User-mode cycles from the CPU's counters, best of 3. The sinks are measured at several densities
of results, because what they cost is mostly the memory they write."""
import os, re, subprocess, sys
B = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build') + '/'
PRE = ('~256 3 * 7 + >t\n' + ' '.join(str((s + c) % 8 if c % 4 != 3 else 3) for c in range(16) for s in range(8)) + ' =d8\n'
       + ' '.join(str((s + c) % 4 if c % 4 != 3 else 3) for c in range(16) for s in range(4)) + ' =d4\n')
def run(line, inp):
    open(B + 'pc.strand', 'w').write(PRE + line + '\n')
    if subprocess.run([B + 'stage2'], stdin=open(B + 'pc.strand', 'rb'), stdout=open(B + 'pc', 'wb')).returncode: sys.exit('does not compile: ' + line)
    os.chmod(B + 'pc', 0o755); best = None
    for _ in range(3):
        o = subprocess.run(['taskset', '-c', str(min(os.sched_getaffinity(0))), B + 'perfprof', B + 'pc'], stdin=open(inp, 'rb'), stderr=subprocess.PIPE,
                           env={'SAMPLE_OUT': '/dev/null', 'PERIOD': '1000000000'}).stderr.decode()
        m = re.search(r'([\d.]+) M cycles, ([\d.]+) M instructions', o); c = (float(m.group(1)), float(m.group(2)))
        if best is None or c < best: best = c
    return best
def main():
    inp = sys.argv[1] if len(sys.argv) > 1 else B + 'pc.in'
    if len(sys.argv) < 2:
        b = open(B + 'gosmall.go', 'rb').read(); open(inp, 'wb').write((b * (1 + (64 << 20) // len(b)))[:64 << 20])
    chunks = os.path.getsize(inp) / 64e6; base = run('bytes $S', inp); N = 12
    print(f'bare line: {base[0] / chunks:.1f} cycles, {base[1] / chunks:.1f} instructions per chunk\n\n| primitive | cycles | instructions |\n|---|---|---|')
    def row(name, line, n=1):
        r = run(line, inp); print(f'| {name} | {(r[0] - base[0]) / chunks / n:.1f} | {(r[1] - base[1]) / chunks / n:.1f} |')
    for name, tok in (('`7 +` add a constant', '7 +'), ('`1 min`', '1 min'), ('`7 =` compare with a constant', '7 ='), ('`7 *` multiply', '7 *'),
                      ('`back +`', 'back +'), ('`peek +`', 'peek +'), ('`@t` table lookup', '@t')):
        row(name, 'bytes ' + ' '.join([tok] * N) + ' $S', N)
    row('`:x x +` tee, push, add', 'bytes ' + ' '.join(f':v{i} v{i} +' for i in range(N)) + ' $S', N)
    for name, sig in (('`/t` 4-state automaton (with a `dup` and a `+`)', '/d4'), ('`\\t` 8-state automaton (with a `dup` and a `+`)', '\\d8')):
        row(name, 'bytes ' + ' '.join([sig + ' dup'] * 3 + [sig]) + ' + + + $S', 4)
    for name, cond in (('nothing', '0 ='), ('an eighth', '7 & 0 ='), ('everything', 'dup =')):
        zero = cond == '0 ='
        row(f'`%x` positions, {name} selected', 'bytes dup ' + ('1 & 2 =' if zero else cond) + ' %w $S')
        row(f'`^x` pack, {name} selected', 'bytes dup dup ' + ('1 & 2 =' if zero else cond) + ' ^p $S')
main()
