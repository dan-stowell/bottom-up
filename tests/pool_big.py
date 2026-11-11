#!/usr/bin/env python3
"""pool_big.py -- the thread pool's one-pass mode on inputs made to defeat its guesses.

A line whose only running sums are the counts of its sinks runs once per range: automata start in a
guessed state, and each range writes its results above where they belong, at a place chosen from the
rate of the block before. Here the guesses are wrong on purpose -- a toggle automaton no warm-up can
predict, and results whose rate jumps from nothing to one per byte -- so the retry, the collision check
and the serial fallback all run. Every program must give the same bytes on 1, 2, 3 and all CPUs."""
import os, random, subprocess, sys
B = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build') + '/'
PROGS = {
 'rate': 'bytes :c dup %w 0 0 c 1 = %v drop drop 3 > $S\nw out4\nv out4\n~1 S + out4\n',
 'toggle': '1 0 0 0  0 1 0 0 =dfa\n~256 :j 7 = >tc\nbytes @tc /dfa :s 0 0 0 s 1 = %w drop drop drop $S\nw out4\n~1 S + out4\n',
 'toggle, pack, prev': '1 0 0 0  0 1 0 0 =dfa\n~256 :j 7 = >tc\nbytes :c @tc /dfa :s 0 0 c s 1 = ^p drop drop dup prev + 0 0 0 0 s %w drop drop drop drop $S\n\'p out\nw out4\n~1 S + out4\n',
}
def inputs(r):
    n = 24 << 20
    yield 'nothing, then everything', bytes(n // 2) + bytes([1]) * (n // 2)
    yield 'runs', b''.join((bytes([r.choice((0, 0, 0, 1, 5, 7))]) * (1 << r.randrange(10, 21))) for _ in range(60))
    yield 'random', r.randbytes(20 << 20).translate(bytes((0, 1, 2, 7, 9)[i % 5] for i in range(256)))
    yield 'rare toggles', bytes(7 if r.random() < 1e-5 else r.choice((0, 1, 5)) for _ in range(1 << 20)) * 13 + b'\x07' + bytes(5 << 20)
def main():
    r = random.Random(3); ncpu = len(os.sched_getaffinity(0)); cpus = sorted(os.sched_getaffinity(0))
    sets = [cpus[:k] for k in (1, 2, 3) if k <= ncpu] + ([cpus] if ncpu > 3 else [])
    ins = list(inputs(r))
    for name, src in PROGS.items():
        open(B + 'pb.strand', 'w').write(src); exe = B + 'pb'
        if subprocess.run([B + 'stage2'], stdin=open(B + 'pb.strand', 'rb'), stdout=open(exe, 'wb')).returncode: sys.exit(f'FAIL pool_big {name}: does not compile')
        os.chmod(exe, 0o755)
        for iname, inp in ins:
            open(B + 'pb.in', 'wb').write(inp); outs = []
            for cs in sets:
                p = subprocess.run(['taskset', '-c', ','.join(map(str, cs)), exe], stdin=open(B + 'pb.in', 'rb'), stdout=subprocess.PIPE)
                outs.append((p.returncode, p.stdout))
            if outs[0][0] or any(o != outs[0] for o in outs): sys.exit(f'FAIL pool_big {name} on "{iname}": {[(o[0], len(o[1])) for o in outs]}')
    # ... and the paths meant to run did run: a copy of the binary with that entry replaced by exit_group(77)
    nm = subprocess.run(['nm', B + 'stub.o'], capture_output=True, text=True, check=True).stdout
    sym = {l.split()[-1]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    patch = bytes.fromhex('b8e7000000bf4d0000000f05'); ins = dict(ins); npath = 0
    for name, iname, label, k, what in (('rate', 'random', 'fcopied', 2, 'pieces copied into place'), ('rate', 'nothing, then everything', 'fcopied', min(4, ncpu), 'pieces copied into place'),
                                        ('toggle', 'random', 'fsretry', min(4, ncpu), 'retry'), 
                                        ('toggle, pack, prev', 'rare toggles', 'fsretry', min(4, ncpu), 'retry')):
        if ncpu < k: continue
        elf = subprocess.run([B + 'stage2'], input=PROGS[name].encode(), capture_output=True).stdout
        at = 112 + sym[label]; open(B + 'pb.in', 'wb').write(ins[iname])
        open(B + 'pbq', 'wb').write(elf[:at] + patch + elf[at + 12:]); os.chmod(B + 'pbq', 0o755)
        st = subprocess.run(['taskset', '-c', ','.join(map(str, cpus[:k])), B + 'pbq'], stdin=open(B + 'pb.in', 'rb'), stdout=subprocess.DEVNULL).returncode
        if st != 77: sys.exit(f'FAIL pool_big path: {name} on "{iname}" with {k} CPUs never reached "{what}" (exit {st})')
        npath += 1
    print(f'PASS pool_big, {npath} path checks ({len(PROGS)} programs x {len(ins)} inputs x {len(sets)} CPU sets)')
main()
