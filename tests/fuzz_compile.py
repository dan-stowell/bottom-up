#!/usr/bin/env python3
"""Differential fuzz of the *compiler*: SIMD compiler binary vs scalar reference, byte for byte.

  fuzz_compile.py --compiler build/stage2 [--n N] [--seed S]
Sources: valid random programs (from fuzz_sem's generator), the same programs padded so that
token/line/chunk boundaries land on every offset mod 16, and mutated (malformed) programs,
for which the exit status must equal the reference's lowest-numbered failing check.
"""
import argparse, os, random, resource, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
resource.setrlimit(resource.RLIMIT_AS, (4 << 30, resource.RLIM_INFINITY))
def unlimit(): resource.setrlimit(resource.RLIMIT_AS, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
import strand
from fuzz_sem import Gen
from spec import *

JUNK = [b'"', b'#', b' ', b'\n', b'\t', b'>', b'@', b'~', b'?', b'!', b'$', b'^', b'&', b'=', b'[', b'{', b'%', b':', b'/', b'0', b'9', b'A', b'z', b'_',
        b'\xff', b'\x00', b'+', b'scan', b'prev', b'scanmax', b'out', b'halt', b'input', b'drop', b'dup', b'sel', b'>x', b'=d', b'1234567890',
        b'x' * 64, b'x' * 65, b'"a b"', b'>scan', b'@out', b'~0', b'!v0', b'\r', b':v0', b'%v0', b':t', b'end', b'%', b':', b'bytes', b'outif', b'input4', b'idx']

def mutate(rnd, src):
    b = bytearray(src)
    for _ in range(rnd.randrange(1, 4)):
        k = rnd.random(); pos = rnd.randrange(len(b) + 1)
        if k < .35: b[pos:pos] = rnd.choice(JUNK)
        elif k < .55 and b: del b[pos % len(b)]
        elif k < .75 and b: b[pos % len(b)] = rnd.randrange(256)
        elif k < .85:
            toks = bytes(b).split(b' ')
            if len(toks) > 2:
                a, c = rnd.randrange(len(toks)), rnd.randrange(len(toks)); toks[a], toks[c] = toks[c], toks[a]
            b = bytearray(b' '.join(toks))
        else:
            lines = bytes(b).split(b'\n'); rnd.shuffle(lines); b = bytearray(b'\n'.join(lines))
    return bytes(b)

def pad(rnd, src):
    """shift everything by 0..17 bytes of leading blank/comment, and vary inter-token spacing"""
    k = rnd.randrange(18)
    lead = rnd.choice([b' ' * k, b'#' + b'c' * k + b'\n', b'\n' * k])
    if rnd.random() < .5: src = src.replace(b' ', b' ' * rnd.randrange(1, 4))
    if rnd.random() < .3: src = src.rstrip(b'\n')          # no trailing newline
    return lead + src

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True); ap.add_argument('--also'); ap.add_argument('--n', type=int, default=500); ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    stats = {}
    cases = [b'', b'\n', b' ', b'#', b'"', b'x', b'>x', b'1 >A', b'1 >A\n', b'input out', b'input out\n\n\n', b'\n\n\ninput out', b'=d\nd out\n', b'"" =d\nd out\n']
    for it in range(a.n):
        if it < len(cases): src = cases[it]
        else:
            src = Gen(rnd).program()
            m = rnd.random()
            if m < .08:                                   # every alignment of the same program
                base = src
                for k in range(1, 17):
                    s2 = b' ' * k + base
                    try: w2 = (0, strand.compile_(s2))
                    except strand.CompileError as e: w2 = (e.code, b'')
                    p2 = subprocess.run([a.compiler], input=s2, capture_output=True, preexec_fn=unlimit, timeout=20)
                    if (p2.returncode, p2.stdout) != w2: open('/tmp/fuzzc_fail.strand', 'wb').write(s2); print('MISMATCH at shift', k); sys.exit(1)
            elif m < .35: src = pad(rnd, src)
            elif m < .85: src = mutate(rnd, pad(rnd, src))
        try: want = (0, strand.compile_(src))
        except strand.CompileError as e: want = (e.code, b'')
        p = subprocess.run([a.compiler], input=src, capture_output=True, preexec_fn=unlimit, timeout=20)
        got = (p.returncode, p.stdout)
        if a.also:      # a second, independently written oracle must agree with the first
            q = subprocess.run([a.also], input=src, capture_output=True, timeout=20)
            if (q.returncode, q.stdout) != want:
                open('/tmp/fuzzc_fail.strand', 'wb').write(src); print('ORACLES DISAGREE iter', it, want[0], q.returncode); sys.exit(1)
        stats[want[0]] = stats.get(want[0], 0) + 1
        if got != want:
            open('/tmp/fuzzc_fail.strand', 'wb').write(src)
            print('MISMATCH iter', it, 'want status', want[0], 'len', len(want[1]), 'got status', got[0], 'len', len(got[1]), p.stderr[:120])
            print(repr(src[:600])); sys.exit(1)
    print('PASS', a.compiler, 'by reference status:', dict(sorted(stats.items())))

if __name__ == '__main__':
    main()
