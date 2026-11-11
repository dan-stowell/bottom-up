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
# a non-forgetting automaton whose functions do not commute: every wrong guess of a state shows
PERM = '0 1 2 3  1 0 2 3  1 2 3 0  0 1 3 2 =d\n'
# the same on 8 states: a transposition, an 8-cycle, a product of transpositions, a 3-cycle
PERM8 = '0 1 2 3 4 5 6 7  1 0 2 3 4 5 6 7  1 2 3 4 5 6 7 0  1 0 3 2 5 4 7 6 =e\n'
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
 'bytesz': 'bytesz dup prev - peek + back2 ^ :x 0 0 x 0 = %z drop drop out\nz out4\n',
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
 'permutation automaton': PERM + 'bytes 3 & /d 48 + out\n',
 'permutation automaton feeding itself': PERM + 'bytes 3 & /d dup /d 4 * + 48 + out\n',
 'three automata in cascade': PERM + 'bytes 3 & /d dup /d dup /d + + 48 + out\n',
 'automaton -> prev -> automaton': PERM + 'bytes 3 & /d 0 over prev + 3 & /d + 48 + out\n',
 'three nested scans': 'input 1 & scan dup 3 & scan dup 7 & scan + + out4\n',
 'four nested scans': 'input 1 & scan dup 3 & scan dup 7 & scan dup 15 & scan + + + out4\n',
 'scanmax of a scan': 'input 100 - scan dup scanmax + out4\n',
 'permutation automaton, 8 states': PERM8 + 'bytes 3 & \\e 48 + out\n',
 '8 states feeding 8 states': PERM8 + 'bytes 3 & \\e dup \\e 8 * + 48 + out\n',
 '8 states -> prev -> 4 states': PERM8 + PERM + 'bytes 3 & \\e 0 over prev + 3 & /d + 48 + out\n',
 'prev of an 8-state automaton, summed': PERM8 + 'bytes 3 & \\e :s 5 = s prev 2 = & $N\n~1 N + out4\n',
 'input4 scan': 'input4 scan out4\n',
 'input4 prev filter': 'input4 dup dup prev > outif4\n',
}
def gen(n, seed=1):
    out = bytearray(); s = seed * 2654435761 % (1 << 32)
    while len(out) < n:
        s = (s * 1664525 + 1013904223) % (1 << 32); r = s >> 24
        out.append(32 if r < 40 else 10 if r < 46 else 97 + r % 26 if r < 250 else r)
    return bytes(out)
allowed = sorted(os.sched_getaffinity(0)); ncpu = len(allowed)        # the CPUs we may really use, not 0..n-1
ids = lambda k: ','.join(map(str, allowed[:k]))
cpusets = sorted({ids(k) for k in (2, 3, 4, ncpu) if 2 <= k <= ncpu}, key=len)
ONE = str(allowed[0])
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
        want = run([exe], files[n], ONE)
        if n <= 4097:
            st, out = strand.interp(src.encode(), open(files[n], 'rb').read())
            assert (st, hashlib.sha256(out).hexdigest()) == want[:2], (name, n, 'serial run disagrees with interpreter')
        for cpus in cpusets:
            # 'chaos': the pool's guesses spoiled with noise (stub.S); the output must not notice
            for mode in ([exe], [exe, 'eager'], [exe, 'chaos', '.'], [exe, 'eager', 'chaos', '.']):
                if len(mode) in (2, 4) and n > 600000: continue
                for pipe in (False, True):
                    if pipe and n > 600000: continue
                    for rep in range(3 if n > 4097 else 1):       # a few repetitions: scheduling varies
                        got = run(mode, files[n], cpus, pipe); nrun += 1
                        if got != want:
                            bad += 1; print('FAIL', name, 'n', n, 'cpus', cpus, mode[1:], 'pipe' if pipe else 'file', got, want)
                            break
# Which path ran? Correct output does not show whether the pool kept a block or redid it on one
# thread (found by the second audit: a chained `prev` fell back while every test passed). A copy of
# each binary with the fallback entry replaced by exit_group(77) answers that. DEPTH is the length
# of the longest chain of carries feeding carries whose first guess fails on this input; the pool
# retries while dry + real + retries stays below the thread count, so a chain of depth d must run in
# parallel on t threads iff d - 1 <= max(0, t - 3), and must fall back otherwise.
DEPTH = {'scan': 1, 'two independent scans': 1, 'bytes words': 1, 'automaton: strip comments': 1,
         'permutation automaton': 1, 'prev of an automaton state': 1, 'prev of a running sum': 1,
         'prev of prev of a running sum': 1, 'prev chain': 1, 'automaton + prev + sum': 1,
         'prev feeding automaton': 1, 'scan feeding scanmax': 1,
         'nested scans (must fall back)': 2, 'permutation automaton feeding itself': 2,
         'automaton -> prev -> automaton': 2, 'scan -> prev -> scan': 2,
         'three automata in cascade': 3, 'three nested scans': 3, 'four nested scans': 4,
         'permutation automaton, 8 states': 1, 'prev of an 8-state automaton, summed': 1,
         '8 states feeding 8 states': 2, '8 states -> prev -> 4 states': 2,
         'scanmax of a scan': 3}       # a running max that misses costs two retries, not one
R = os.path.join(H, '..', 'build')
nm = subprocess.run(['nm', R + '/stub.o'], capture_output=True, text=True, check=True).stdout
fsbad = [int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == 'fsbad'][0]
stub = open(R + '/stub.bin', 'rb').read(); patch = bytes.fromhex('b8e7000000bf4d0000000f05')
npath = 0
for name, depth in DEPTH.items():
    elf = subprocess.run([comp], input=PROGS[name].encode(), capture_output=True).stdout
    at = 112 + fsbad
    assert elf[at:at + 12] == stub[fsbad:fsbad + 12], 'stub layout changed: fix the fallback detector'
    exe = f'{T}/q'; open(exe, 'wb').write(elf[:at] + patch + elf[at + 12:]); os.chmod(exe, 0o755)
    for cpus in cpusets:
        t = cpus.count(',') + 1
        st = subprocess.run(['taskset', '-c', cpus, exe], stdin=open(files[sizes[-1]], 'rb'), stdout=subprocess.DEVNULL).returncode
        want = 0 if depth - 1 <= max(0, t - 3) else 77; npath += 1
        if st != want:
            bad += 1; print('FAIL path', name, 'threads', t, 'exit', st, 'expected', want)
print('PASS' if not bad else 'FAILED', comp, len(PROGS), 'programs,', nrun, 'pool runs,', npath, 'path checks')
sys.exit(1 if bad else 0)
