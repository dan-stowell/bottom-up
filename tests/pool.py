#!/usr/bin/env python3
"""pool.py COMPILER -- deterministic tests of the thread pool at production block sizes.
Each program runs on inputs big enough for several real blocks, on 2, 3, 4 and all CPUs, from a
file (mmap path) and from a pipe; the same binary pinned to one CPU (the pool never forks there)
is the oracle, and for small inputs so is the scalar interpreter, in normal and eager mode.
Programs target what the random fuzzer rarely builds (several found by Codex, REVIEWS.md review 3)."""
import os, subprocess, sys, hashlib
H = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(H, '..', 'boot'))
import strand
comp = sys.argv[1]; T = '/tmp/strand_pool'; os.makedirs(T, exist_ok=True)
PROGS = {
 'persistent tee re-read': 'input :x x prev + >y\nx end\ny out4\n',
 'tee then prev, two outputs': 'input :a dup prev :b + >c\na b + c + out4\n',
 'prev chain': 'input dup prev over over prev + + dup prev - + out\n',
 'scan': 'input scan out4\n',
 'scanmax of negatives': 'input 200 - scanmax out4\n',
 'scan overflow': 'input 16777216 * scan out4\n',
 'two independent scans': 'input dup scan swap 3 * scanmax + out4\n',
 'nested scans (must fall back)': 'input scan dup 1 & scan + out4\n',
 'scan -> prev -> scan': 'input 1 & scan dup prev 3 & dup scan + + out4\n',
 'scan feeding scanmax': 'input 10 = :n scan idx n * scanmax + $S\n~1 S + out4\n',
 'pending output before fork': '~7 65 + out\ninput 1 + out\n~3 48 + out\ninput scan out4\n',
 'sum and max': 'input $S\ninput ^M\n~1 S + M + out4\n',
 'filter': 'input dup 3 & 0 = outif\n',
 'filter4 after scan': 'input scan dup 5 & 1 = outif4\n',
 'gather': '~256 255 swap - >t\ninput @t out\n',
 'store then read next line': 'input 3 * >a\na a prev - >b\nb out4\n',
 'bytes prev': 'bytes dup prev - out\n',
 'bytes words': 'bytes 32 > dup prev 1 ^ & $W\n~1 W + out4\n',
 'bytes filter': 'bytes dup 97 >= over 122 <= & outif\n',
 'bytes table': '~256 3 * 7 + >t\nbytes @t :c c prev ^ out\n',
 'automaton: strip comments': '~256 :i 35 = i 34 = 2 * + i 10 = 3 * + i 92 = 4 * + >cls\n0 1 2 2  1 1 2 2  2 1 0 2  0 0 2 2  0 1 3 2 =dfa\nbytes dup @cls /dfa 1 != outif\n',
 'automaton: states out': '1 2 3 0 2 2 0 1 3 0 1 1 0 3 2 2 =d\nbytes 3 & /d 48 + out\n',
 'automaton + prev + sum': '1 2 3 0 2 2 0 1 3 0 1 1 0 3 2 2 =d\nbytes 3 & /d dup prev 4 * + $S\n~1 S + out4\n',
 'two automata': '1 2 3 0 2 2 0 1 3 0 1 1 0 3 2 2 =d\n0 0 1 1 3 2 1 0 =e\nbytes dup 1 & /e swap dup 3 & /d 4 * + + out\n',
 'automaton feeding automaton (must fall back)': '1 2 3 0 2 2 0 1 3 0 1 1 0 3 2 2 =d\nbytes 3 & /d dup /d 4 * + 48 + out\n',
 'prev of an automaton state': '1 2 3 0 2 2 0 1 3 0 1 1 0 3 2 2 =d\nbytes 3 & /d :s 2 = s prev 0 = & $N\n~1 N + out4\n',
 'prev of state, filtered out': '1 2 3 0 2 2 0 1 3 0 1 1 0 3 2 2 =d\nbytes dup 3 & /d dup prev != outif\n',
 'prev of a running sum': 'input 1 & scan dup prev 2 * + out4\n',
 'prev of prev of a running sum': 'input 3 & scan dup prev dup prev + + out4\n',
 'prev feeding automaton': '1 2 3 0 2 2 0 1 3 0 1 1 0 3 2 2 =d\nbytes dup prev + 3 & /d 48 + out\n',
 'input4 scan': 'input4 scan out4\n',
 'input4 prev filter': 'input4 dup dup prev > outif4\n',
}
def gen(n, seed=1):
    out = bytearray(); s = seed * 2654435761 % (1 << 32)
    while len(out) < n:
        s = (s * 1664525 + 1013904223) % (1 << 32); r = s >> 24
        out.append(32 if r < 40 else 10 if r < 46 else 97 + r % 26 if r < 250 else r)
    return bytes(out)
ncpu = len(os.sched_getaffinity(0))
cpusets = ['0-1', '0-2', '0-3', '0-%d' % (ncpu - 1)]
sizes = [64 * ncpu - 1, 64 * ncpu, 64 * ncpu + 1, 4097, 524288 + 1, 9 * (1 << 20) + 77]     # last: >2 scan blocks, >2 out blocks
files = {}
for n in sizes:
    files[n] = f'{T}/in{n}'; open(files[n], 'wb').write(gen(n))
def run(cmd, path, cpus, pipe=False):
    if pipe:
        p = subprocess.run(['taskset', '-c', cpus] + cmd, input=open(path, 'rb').read(), capture_output=True)
    else:
        p = subprocess.run(['taskset', '-c', cpus] + cmd, stdin=open(path, 'rb'), capture_output=True)
    return p.returncode, hashlib.sha256(p.stdout).hexdigest(), len(p.stdout)
bad = 0; nrun = 0
for name, src in PROGS.items():
    exe = f'{T}/p'
    b = subprocess.run([comp], input=src.encode(), capture_output=True)
    assert b.returncode == 0, (name, 'does not compile', b.returncode)
    open(exe, 'wb').write(b.stdout); os.chmod(exe, 0o755)
    for n in sizes:
        want = run([exe], files[n], '0')
        if n <= 4097:
            st, out = strand.interp(src.encode(), open(files[n], 'rb').read())
            assert (st, hashlib.sha256(out).hexdigest()) == want[:2], (name, n, 'serial run disagrees with interpreter')
        for cpus in cpusets:
            for mode in ([exe], [exe, 'eager']):
                if len(mode) == 2 and n > 600000: continue
                for pipe in (False, True):
                    if pipe and n > 600000: continue
                    for rep in range(3 if n > 4097 else 1):       # a few repetitions: scheduling varies
                        got = run(mode, files[n], cpus, pipe); nrun += 1
                        if got != want:
                            bad += 1; print('FAIL', name, 'n', n, 'cpus', cpus, mode[1:], 'pipe' if pipe else 'file', got, want)
                            break
print('PASS' if not bad else 'FAILED', comp, len(PROGS), 'programs,', nrun, 'pool runs')
sys.exit(1 if bad else 0)
