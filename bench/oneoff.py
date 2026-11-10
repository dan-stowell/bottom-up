#!/usr/bin/env python3
"""oneoff.py -- from source text to answer: compile, then run once, for a question asked once.
Strand: build/stage2 < program > exe, then exe < file. C: gcc -O3 -march=native [-fopenmp] on
bench/base.c / base_omp.c with the same kernel, then run. One CPU for everything (taskset), the
input in the page cache, output to /dev/null, wall time of both steps together, median of 7.
Inputs are prefixes of the benchmark fixtures in build/suite (run bench/run.py first)."""
import glob, os, statistics, subprocess, sys, time
H = os.path.dirname(os.path.abspath(__file__)); R = os.path.join(H, '..'); B = os.path.join(R, 'build', 'suite'); T = os.path.join(B, 'oneoff'); os.makedirs(T, exist_ok=True)
cpu = str(sorted(os.sched_getaffinity(0))[-1]); ts = ['taskset', '-c', cpu]; env = dict(os.environ, OMP_NUM_THREADS='1')
progs = {}
for line in open(f'{H}/programs.txt'):
    if line.startswith('#') or not line.strip(): continue
    name, kind, shape, prog = [x.strip() for x in line.split('|', 3)]; progs[name] = (kind, '\n'.join(x.strip() for x in prog.split(';')) + '\n')
def timed(steps, inp):
    v = []
    for _ in range(7):
        t = time.perf_counter()
        for cmd, stdin, stdout in steps:
            subprocess.run(ts + cmd, stdin=open(stdin or inp, 'rb'), stdout=open(stdout or '/dev/null', 'wb'), check=True, env=env)
        v.append(time.perf_counter() - t)
    return statistics.median(v) * 1e3
print('| program | input | Strand: compile + run, ms | gcc -O3 base.c + run, ms | gcc -O3 -fopenmp base_omp.c + run, ms |\n|---|---|---|---|---|')
for name in sys.argv[1:] or ['words', 'strip', 'json']:
    kind, src = progs[name]; full = glob.glob(f'{B}/{kind}256-*')[0]
    open(f'{T}/{name}.strand', 'w').write(src)
    open(f'{T}/st', 'wb').close(); os.chmod(f'{T}/st', 0o755)       # the compiler's output is written into an executable file
    for mib in (1, 16, 256):
        inp = f'{T}/{kind}{mib}'
        if not os.path.exists(inp): open(inp, 'wb').write(open(full, 'rb').read(mib << 20))
        st = timed([([f'{R}/build/stage2'], f'{T}/{name}.strand', f'{T}/st'), ([f'{T}/st'], None, None)], inp)
        c = timed([(['gcc', '-O3', '-march=native', '-w', f'-DK=P_{name}', '-o', f'{T}/c', f'{H}/base.c'], '/dev/null', None), ([f'{T}/c'], None, None)], inp)
        o = timed([(['gcc', '-O3', '-march=native', '-fopenmp', '-w', f'-DK=P_{name}', '-o', f'{T}/o', f'{H}/base_omp.c'], '/dev/null', None), ([f'{T}/o'], None, None)], inp)
        print(f'| {name} | {mib} MiB | {st:.1f} | {c:.1f} | {o:.1f} |', flush=True)
