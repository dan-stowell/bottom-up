#!/usr/bin/env python3
"""lineprof.py INPUT [COMPILER] -- where does the self-hosted compiler spend its time?
(Written before tools/perfprof.c, which uses the CPU's counters and is the better instrument.) This one works by timing: compile every prefix of src/compiler.strand (each line is one
loop and a prefix is a valid program), run each prefix on INPUT, and report the differences between
consecutive prefixes: user ms, system ms, page faults, grouped by the section comments. Vectors die
earlier in a truncated program than in the full one, so memory reuse differs slightly; the totals
of the last prefix are the real ones."""
import os, resource, subprocess, sys
H = os.path.dirname(os.path.abspath(__file__)); R = os.path.join(H, '..')
inp = sys.argv[1]; comp = sys.argv[2] if len(sys.argv) > 2 else f'{R}/build/stage2'
tables = open(f'{R}/src/tables.strand').read(); src = open(f'{R}/src/compiler.strand').read().split('\n')
cpu = sorted(os.sched_getaffinity(0))[-1]
def measure(exe, reps=5):
    best = None
    for _ in range(reps):
        pid = os.fork()
        if pid == 0:
            os.sched_setaffinity(0, {cpu}); os.dup2(os.open(inp, os.O_RDONLY), 0); os.dup2(os.open('/dev/null', os.O_WRONLY), 1); os.execv(exe, [exe])
        _, st, ru = os.wait4(pid, 0)
        v = (ru.ru_utime * 1e3, ru.ru_stime * 1e3, ru.ru_minflt)
        if best is None or v[0] + v[1] < best[0] + best[1]: best = v
    return best
rows = []; sect = 'setup'; prev = (0, 0, 0); exe = '/tmp/strand_lineprof'
for k, line in enumerate(src):
    if line.startswith('# -----'): sect = line.strip('# -').split(':')[0].strip()
    if not line.strip() or line.startswith('#'): continue
    b = subprocess.run([comp], input=(tables + '\n'.join(src[:k + 1]) + '\n').encode(), capture_output=True)
    if b.returncode: print('prefix', k + 1, 'does not compile:', b.returncode); continue
    open(exe, 'wb').write(b.stdout); os.chmod(exe, 0o755)
    m = measure(exe); rows.append((sect, k + 1, line, tuple(a - p for a, p in zip(m, prev)))); prev = m
print(f'{"line":>4} {"ms":>7} {"faults":>7}  source      (user + system; the split between them is too coarse to print per line)')
tot = {}
for sect, k, line, d in rows:
    t = tot.setdefault(sect, [0, 0, 0]); [t.__setitem__(i, t[i] + d[i]) for i in range(3)]
    if d[0] + d[1] >= 0.8 or d[2] >= 60: print(f'{k:>4} {d[0] + d[1]:7.2f} {d[2]:7.0f}  {line[:140]}')
print(f'\n{"ms":>7} {"faults":>7}  section')
for sect, t in tot.items(): print(f'{t[0] + t[1]:7.2f} {t[2]:7.0f}  {sect}')
print(f'{prev[0] + prev[1]:7.2f} {prev[2]:7.0f}  total ({prev[0]:.1f} user, {prev[1]:.1f} system)')
