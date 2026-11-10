#!/usr/bin/env python3
"""run.py [MiB] -- build and time every program in programs.txt as a whole process:
scalar C, gcc -O3, Strand on 1 core and on all cores. Checks all four produce the same bytes.
Prints a markdown table of GB/s (input bytes per second of wall time, best of N)."""
import os, subprocess, sys, hashlib
H = os.path.dirname(os.path.abspath(__file__)); R = os.path.join(H, '..'); B = os.path.join(R, 'build', 'suite')
os.makedirs(B, exist_ok=True)
mib = int(sys.argv[1]) if len(sys.argv) > 1 else 256
reps = 5
def sh(*a, **k): return subprocess.run(a, check=True, **k)
sh('gcc', '-O2', '-o', f'{B}/gen', f'{H}/gen.c'); sh('gcc', '-O2', '-o', f'{B}/bench', f'{R}/tools/bench.c')
data = {'text': f'{B}/text{mib}', 'ints': f'{B}/ints{mib}'}
if not os.path.exists(data['text']): sh(f'{B}/gen', str(mib), stdout=open(data['text'], 'wb'))
if not os.path.exists(data['ints']):
    import random
    r = random.Random(7); blk = bytearray()
    with open(data['ints'], 'wb') as f:
        chunk = b''.join(int(r.gauss(0, 1000)).to_bytes(4, 'little', signed=True) for _ in range(1 << 18))
        for _ in range(mib): f.write(chunk)
ncpu = len(os.sched_getaffinity(0))
def gbps(cmd, path, cpus):
    out = subprocess.run(['taskset', '-c', cpus, f'{B}/bench', str(reps), path, cmd], capture_output=True, text=True).stdout
    return os.path.getsize(path) / float(out.split()[2]) / 1e3
def digest(cmd, path):
    h = hashlib.sha256(); p = subprocess.Popen([cmd], stdin=open(path, 'rb'), stdout=subprocess.PIPE)
    for b in iter(lambda: p.stdout.read(1 << 20), b''): h.update(b)
    assert p.wait() == 0, cmd
    return h.hexdigest()
print(f'input {mib} MiB from the page cache, output to /dev/null, best of {reps}, GB/s of input; {ncpu} vCPUs\n')
print('| program | shape | scalar C | gcc -O3 | Strand, 1 core | Strand, 4 cores | Strand, all | vs scalar | vs gcc -O3 |\n|---|---|---|---|---|---|---|---|---|')
for line in open(f'{H}/programs.txt'):
    if line.startswith('#') or not line.strip(): continue
    name, kind, shape, prog = [x.strip() for x in line.split('|', 3)]
    src = '\n'.join(x.strip() for x in prog.split(';')) + '\n'
    open(f'{B}/{name}.strand', 'w').write(src)
    sh(f'{R}/build/stage2', stdin=open(f'{B}/{name}.strand'), stdout=open(f'{B}/st_{name}', 'wb')); os.chmod(f'{B}/st_{name}', 0o755)
    sh('gcc', '-O2', '-fno-tree-vectorize', f'-DK=P_{name}', '-o', f'{B}/c_{name}', f'{H}/base.c')
    sh('gcc', '-O3', '-march=native', f'-DK=P_{name}', '-o', f'{B}/v_{name}', f'{H}/base.c')
    d = {digest(f'{B}/{x}_{name}', data[kind]) for x in ('c', 'v', 'st')}
    assert len(d) == 1, ('outputs differ', name)
    c, v = gbps(f'{B}/c_{name}', data[kind], '3'), gbps(f'{B}/v_{name}', data[kind], '3')
    s1, s4, s8 = gbps(f'{B}/st_{name}', data[kind], '3'), gbps(f'{B}/st_{name}', data[kind], '0-3'), gbps(f'{B}/st_{name}', data[kind], f'0-{ncpu-1}')
    best = max(s1, s4, s8)
    print(f'| {name} | {shape} | {c:.2f} | {v:.2f} | {s1:.2f} | {s4:.2f} | {s8:.2f} | {best/c:.1f}x | {best/v:.1f}x |', flush=True)
