#!/usr/bin/env python3
"""run.py [MiB] [--only a,b] [--rounds R] -- time every program in programs.txt as a whole process.

Up to four implementations of each program, all reading the same cached file, output to /dev/null:
  plain C    bench/base.c, gcc -O3 -march=native, one thread, read() in 64 KiB blocks
  OpenMP C   bench/base_omp.c, gcc -O3 -march=native -fopenmp, mmap; C without intrinsics, shaped to vectorise
  hand C     bench/hand.c, AVX-512 intrinsics + OpenMP, for the programs where the other two lose
  Strand     the program text in programs.txt, compiled by build/stage2
All but plain C run on 1, 4 and all allowed CPUs. Every timed configuration's output is hashed
under that same CPU mask and must equal plain C's. Trials are interleaved in random order, every
sample is kept (build/suite/results.json), and the table reports the median and the best.
"Best C" for a CPU budget is the fastest median of any C configuration using at most that many CPUs;
ordinary C (plain, OpenMP) and hand C are reported separately.
Inputs are named after a hash of their generators, so a changed generator never reuses an old file;
results.json records input hashes, compiler version and source revision.
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
sys.path.insert(0, H); import jsongen
gh = hashlib.sha256(open(f'{H}/gen.c', 'rb').read() + open(__file__, 'rb').read() + open(f'{H}/jsongen.py', 'rb').read()).hexdigest()[:8]
data = {k: f'{B}/{k}{mib}-{gh}' for k in ('text', 'ints', 'code', 'csv', 'json')}
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
# JSON: one valid document of about 16 MiB, repeated. What its structural characters are is taken from
# Python's parser (jsongen.skeleton), not from any of the scanners being timed.
unit = jsongen.document(min(mib, 16) << 20); reps = max(1, mib // 16)
if not os.path.exists(data['json']):
    with open(data['json'], 'wb') as f:
        for _ in range(reps): f.write(unit)
independent = {'json': hashlib.sha256(jsongen.skeleton(unit) * reps).hexdigest()}
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
def filehash(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''): h.update(b)
    return h.hexdigest()
def out(*a): return subprocess.run(a, capture_output=True, text=True).stdout.strip()
meta = {'mib': mib, 'cpus': allowed, 'sets': sets, 'rounds': rounds, 'unit': 'microseconds wall per run',
        'inputs': {k: filehash(v) for k, v in data.items()}, 'gcc': out('gcc', '--version').split('\n')[0],
        'revision': out('git', '-C', R, 'rev-parse', 'HEAD'), 'dirty': bool(out('git', '-C', R, 'status', '--porcelain', '--untracked-files=no')),
        'stage2': filehash(f'{R}/build/stage2')}
HAND = set(__import__('re').findall(r'#define P_(\w+)', open(f'{H}/hand.c').read()))
med = lambda v: sorted(v)[len(v) // 2]
rnd = random.Random(1); results = {}; order_s = list(sets)
print(f'input {mib} MiB from the page cache, output to /dev/null, {rounds} interleaved rounds, GB/s of input as median (best); CPUs {allowed}\n')
print('| program | shape | plain C | ' + ' | '.join(f'OpenMP C, {s}' for s in sets) + ' | ' + ' | '.join(f'hand C, {s}' for s in sets) + ' | ' + ' | '.join(f'Strand, {s}' for s in sets)
      + ' | Strand / best ordinary C (' + ', '.join(sets) + ') | Strand / best hand C (' + ', '.join(sets) + ') |')
print('|' + '---|' * (5 + 3 * len(sets)))
for line in open(f'{H}/programs.txt'):
    if line.startswith('#') or not line.strip(): continue
    name, kind, shape, prog = [x.strip() for x in line.split('|', 3)]
    if only and name not in only: continue
    src = '\n'.join(x.strip() for x in prog.split(';')) + '\n'
    open(f'{B}/{name}.strand', 'w').write(src)
    sh(f'{R}/build/stage2', stdin=open(f'{B}/{name}.strand'), stdout=open(f'{B}/st_{name}', 'wb')); os.chmod(f'{B}/st_{name}', 0o755)
    sh('gcc', '-O3', '-march=native', '-w', f'-DK=P_{name}', '-o', f'{B}/c_{name}', f'{H}/base.c')
    sh('gcc', '-O3', '-march=native', '-fopenmp', '-w', f'-DK=P_{name}', '-o', f'{B}/omp_{name}', f'{H}/base_omp.c')
    cols = [('c', '1')] + [(k, s) for k in (('omp', 'hand', 'st') if name in HAND else ('omp', 'st')) for s in sets]
    if name in HAND: sh('gcc', '-O3', '-march=native', '-fopenmp', '-w', f'-DK=P_{name}', '-o', f'{B}/hand_{name}', f'{H}/hand.c')
    path = data[kind]; size = os.path.getsize(path)
    want = digest(f'{B}/c_{name}', path, sets['1'])
    if name in independent and want != independent[name]: raise SystemExit(f'{name}: plain C disagrees with the independent reference')
    for k, s in cols[1:]:
        if digest(f'{B}/{k}_{name}', path, sets[s]) != want: raise SystemExit(f'{name}: output of {k} on {s} CPUs differs from plain C')
    t = {c: [] for c in cols}
    for _ in range(rounds):
        order = cols[:]; rnd.shuffle(order)
        for k, s in order: t[(k, s)].append(once(f'{B}/{k}_{name}', path, sets[s]))
    g = {c: (size / med(v) / 1e3, size / min(v) / 1e3) for c, v in t.items()}
    results[name] = {f'{k}:{s}': v for (k, s), v in t.items()}
    cell = lambda c: f'{g[c][0]:.2f} ({g[c][1]:.2f})' if c in g else '-'
    def best(kinds, s):         # fastest median among these kinds on at most this many CPUs
        return max((g[(k, u)][0] for k in kinds for u in order_s[:order_s.index(s) + 1] if (k, u) in g), default=0)
    ro = ' / '.join(f'{g[("st", s)][0] / best(("c", "omp"), s):.2f}x' for s in sets)
    rh = ' / '.join(f'{g[("st", s)][0] / best(("hand",), s):.2f}x' for s in sets) if name in HAND else '-'
    print(f'| {name} | {shape} | {cell(("c", "1"))} | ' + ' | '.join(cell((k, s)) for k in ('omp', 'hand', 'st') for s in sets) + f' | {ro} | {rh} |', flush=True)
    json.dump(dict(meta, samples=results), open(f'{B}/results.json', 'w'), indent=1)
