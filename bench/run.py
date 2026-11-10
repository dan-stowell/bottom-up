#!/usr/bin/env python3
"""run.py [MiB] [--only a,b] [--rounds R] -- time every program in programs.txt as a whole process.

Three implementations of each program, all reading the same cached file, output to /dev/null:
  plain C    bench/base.c, gcc -O3 -march=native, one thread, read() in 64 KiB blocks
  OpenMP C   bench/base_omp.c, gcc -O3 -march=native -fopenmp, mmap; the strongest C without intrinsics
  Strand     the program text in programs.txt, compiled by build/stage2
OpenMP C and Strand run on 1, 4 and all allowed CPUs. Every timed configuration's output is hashed
under that same CPU mask and must equal plain C's. Trials are interleaved in random order, every
sample is kept (build/suite/results.json), and the table reports the median and the best.
Timing boundary: fork to exit of the child (tools/bench.c), so process start, mapping or reading
the input, page faults, computation, writes and exit are all inside."""
import hashlib, json, os, random, subprocess, sys
H = os.path.dirname(os.path.abspath(__file__)); R = os.path.join(H, '..'); B = os.path.join(R, 'build', 'suite')
os.makedirs(B, exist_ok=True)
args = sys.argv[1:]; only = None; rounds = 7
if '--only' in args: i = args.index('--only'); only = args[i + 1].split(','); del args[i:i + 2]
if '--rounds' in args: i = args.index('--rounds'); rounds = int(args[i + 1]); del args[i:i + 2]
mib = int(args[0]) if args else 256
def sh(*a, **k): return subprocess.run(a, check=True, **k)
sh('gcc', '-O2', '-o', f'{B}/gen', f'{H}/gen.c'); sh('gcc', '-O2', '-o', f'{B}/bench', f'{R}/tools/bench.c')
data = {k: f'{B}/{k}{mib}' for k in ('text', 'ints', 'code', 'csv')}
for k in ('text', 'csv'):
    if not os.path.exists(data[k]): sh(f'{B}/gen', str(mib), k, stdout=open(data[k], 'wb'))
if not os.path.exists(data['ints']):
    r = random.Random(7)
    with open(data['ints'], 'wb') as f:
        for _ in range(mib // 16 or 1):                       # 16 MiB of fresh values per period
            f.write(b''.join(int(r.gauss(0, 1000)).to_bytes(4, 'little', signed=True) for _ in range(1 << 22))[:mib << 20])
if not os.path.exists(data['code']):                          # real source text: the Python standard library, repeated
    import glob, sysconfig
    srcs = sorted(glob.glob(os.path.join(sysconfig.get_paths()['stdlib'], '*.py')))
    blob = b''.join(open(p, 'rb').read() for p in srcs) or open(data['text'], 'rb').read()
    with open(data['code'], 'wb') as f:
        n = 0
        while n < mib << 20: f.write(blob[:(mib << 20) - n]); n += len(blob)
allowed = sorted(os.sched_getaffinity(0))
sets = {'1': [allowed[min(3, len(allowed) - 1)]]}
if len(allowed) >= 4: sets['4'] = allowed[:4]
if len(allowed) > 4: sets['all'] = allowed
def env(cpus): return dict(os.environ, OMP_NUM_THREADS=str(len(cpus)), OMP_DYNAMIC='FALSE', OMP_PROC_BIND='FALSE')
def tset(cpus): return ['taskset', '-c', ','.join(map(str, cpus))]
def digest(exe, path, cpus):
    h = hashlib.sha256(); p = subprocess.Popen(tset(cpus) + [exe], stdin=open(path, 'rb'), stdout=subprocess.PIPE, env=env(cpus))
    for b in iter(lambda: p.stdout.read(1 << 20), b''): h.update(b)
    if p.wait() != 0: raise SystemExit(f'{exe} failed on {path}')
    return h.hexdigest()
def once(exe, path, cpus):
    p = subprocess.run(tset(cpus) + [f'{B}/bench', '1', path, exe], capture_output=True, text=True, env=env(cpus))
    if p.returncode != 0: raise SystemExit(f'timing {exe} failed: {p.stderr}')
    return float(p.stdout.split()[2])
med = lambda v: sorted(v)[len(v) // 2]
rnd = random.Random(1); results = {}; cols = [('c', '1')] + [(k, s) for k in ('omp', 'st') for s in sets]
print(f'input {mib} MiB from the page cache, output to /dev/null, {rounds} interleaved rounds, GB/s of input as median (best); CPUs {allowed}\n')
print('| program | shape | plain C | ' + ' | '.join(f'OpenMP C, {s}' for s in sets) + ' | ' + ' | '.join(f'Strand, {s}' for s in sets) + ' | ' + ' | '.join(f'Strand / best C, {s}' for s in sets) + ' |')
print('|' + '---|' * (3 + 3 * len(sets)))
for line in open(f'{H}/programs.txt'):
    if line.startswith('#') or not line.strip(): continue
    name, kind, shape, prog = [x.strip() for x in line.split('|', 3)]
    if only and name not in only: continue
    src = '\n'.join(x.strip() for x in prog.split(';')) + '\n'
    open(f'{B}/{name}.strand', 'w').write(src)
    sh(f'{R}/build/stage2', stdin=open(f'{B}/{name}.strand'), stdout=open(f'{B}/st_{name}', 'wb')); os.chmod(f'{B}/st_{name}', 0o755)
    sh('gcc', '-O3', '-march=native', '-w', f'-DK=P_{name}', '-o', f'{B}/c_{name}', f'{H}/base.c')
    sh('gcc', '-O3', '-march=native', '-fopenmp', '-w', f'-DK=P_{name}', '-o', f'{B}/omp_{name}', f'{H}/base_omp.c')
    path = data[kind]; size = os.path.getsize(path)
    want = digest(f'{B}/c_{name}', path, sets['1'])
    for k, s in cols[1:]:
        if digest(f'{B}/{k}_{name}', path, sets[s]) != want: raise SystemExit(f'{name}: output of {k} on {s} CPUs differs from plain C')
    t = {c: [] for c in cols}
    for _ in range(rounds):
        order = cols[:]; rnd.shuffle(order)
        for k, s in order: t[(k, s)].append(once(f'{B}/{k}_{name}', path, sets[s]))
    g = {c: (size / med(v) / 1e3, size / min(v) / 1e3) for c, v in t.items()}
    results[name] = {f'{k}:{s}': v for (k, s), v in t.items()}
    cell = lambda c: f'{g[c][0]:.2f} ({g[c][1]:.2f})'
    ratio = lambda s: g[('st', s)][0] / max(g[('omp', s)][0], g[('c', '1')][0] if s == '1' else 0)
    print(f'| {name} | {shape} | {cell(("c", "1"))} | ' + ' | '.join(cell(('omp', s)) for s in sets) + ' | ' + ' | '.join(cell(('st', s)) for s in sets) + ' | ' + ' | '.join(f'{ratio(s):.2f}x' for s in sets) + ' |', flush=True)
    json.dump({'mib': mib, 'cpus': allowed, 'unit': 'microseconds wall per run', 'samples': results}, open(f'{B}/results.json', 'w'), indent=1)
