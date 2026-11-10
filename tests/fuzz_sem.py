#!/usr/bin/env python3
"""Differential fuzz of *semantics*: random Strand programs, compiled binary vs scalar interpreter.

  fuzz_sem.py [--compiler PATH] [--n N] [--seed S]
--compiler: a Strand compiler binary (reads source on stdin, writes ELF on stdout).
            Default: the Python reference compiler (tests the stencils themselves).
Input lengths concentrate on chunk boundaries (multiples of 16 +/- 1).
"""
import argparse, os, random, resource, subprocess, sys, tempfile
# An earlier version of this fuzzer OOM-killed the machine (a random program did `~N` with N ~ 1e9).
# Guard: cap this process's address space; children lift the cap (they mmap 64 GiB lazily) but get a timeout.
resource.setrlimit(resource.RLIMIT_AS, (4 << 30, resource.RLIM_INFINITY))
def unlimit(): resource.setrlimit(resource.RLIMIT_AS, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
from spec import *

BIN = ['+', '-', '*', '&', '|', '^', 'min', 'max', '<', '<=', '>', '>=', '=', '!=']
LENS = [0, 1, 2, 3, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64, 65, 79, 80, 81, 127, 128, 129, 255, 256, 257, 1000]

class Gen:
    def __init__(self, rnd):
        self.r = rnd; self.vecs = {}; self.scal = []; self.lines = []; self.nv = 0; self.ns = 0; self.cls = 0
    def newcls(self): self.cls += 1; return self.cls
    def lit(self):
        r = self.r
        return str(r.choice([0, 1, 2, 3, 7, 15, 16, 17, 31, 32, 33, 64, 255, 256, 65535, 999999999, r.randrange(1000)]))
    def scalar(self):
        return self.r.choice(self.scal) if self.scal and self.r.random() < .5 else self.lit()
    def expr(self, lcls, depth_budget=7):
        """tokens after the lead; stack starts at depth 1; ends at depth 1 (or 2 if want2)."""
        r = self.r; toks = []; d = 1; used = set()
        same = [v for v, c in self.vecs.items() if c == lcls]
        for _ in range(r.randrange(0, 12)):
            ch = r.random()
            if ch < .25 and d < depth_budget:
                if same and r.random() < .6: toks.append(r.choice(same))
                elif self.vecs and r.random() < .004: toks.append(r.choice(list(self.vecs)))   # likely length mismatch
                elif r.random() < .2 and self.vecs: toks.append('&' + r.choice(list(self.vecs)))
                else: toks.append(self.scalar())
                d += 1
            elif ch < .55 and d >= 2:
                op = r.choice(BIN + (['<<', '>>'] if r.random() < .3 else []))
                if op in ('<<', '>>'): toks += [self.r.choice(['31', '63', '7']), '&', op] if d < depth_budget else [op]
                else: toks.append(op)
                d -= 1
            elif ch < .62 and d >= 3: toks.append('sel'); d -= 2
            elif ch < .80:
                op = r.choice(['scan', 'scanmax', 'prev'])
                key = ('m' if op == 'scanmax' else 's', d)
                if key not in used: used.add(key); toks.append(op)
            elif ch < .85 and d < depth_budget: toks.append(r.choice(['dup', 'over'] if d >= 2 else ['dup'])); d += 1
            elif ch < .88 and d >= 2: toks.append('swap')
            elif ch < .90 and d >= 2: toks.append('drop'); d -= 1
            elif self.vecs and d < depth_budget - 2:
                v = r.choice(list(self.vecs))
                if r.random() < .985: toks += ["0", "max", "&" + v, "1", "-", "min", "@" + v]   # clamped
                else: toks.append('@' + v)                                                 # may fail: fine
        while d > 1: toks.append(r.choice(BIN)); d -= 1
        return toks
    def line(self):
        r = self.r
        ch = r.random()
        if ch < .08:
            v = 'd%d' % self.nv; self.nv += 1
            items = [self.lit() if r.random() < .7 else '"%s"' % ''.join(r.choice('ab c!xyz') for _ in range(r.randrange(0, 6))) for _ in range(r.randrange(0, 40))]
            self.lines.append(' '.join(items + ['=' + v])); self.vecs[v] = self.newcls(); return
        if ch < .22:   # scalar line
            toks = [self.scalar() if r.random() < .7 or not self.vecs else '&' + r.choice(list(self.vecs))] + self.expr(-1)
            if r.random() < .06 and self.scal: toks.append('halt')
            else:
                s = 'S%d' % self.ns; self.ns += 1; toks.append('>' + s); self.scal.append(s)
            self.lines.append(' '.join(toks)); return
        # vector line
        lead = r.random()
        if lead < .45 and self.vecs:
            v = r.choice(list(self.vecs)); lcls = self.vecs[v]; toks = [v]
        elif lead < .75: toks = ['input']; lcls = 0
        else:
            s = r.choice(self.scal) if self.scal and r.random() < .4 else r.choice(['0', '1', '15', '16', '17', '33', '100'])
            toks = ['~' + s]; lcls = ('iota', s) if s[0] != 'S' else self.newcls()
        toks += self.expr(lcls)
        k = r.random()
        if k < .45:
            v = 'v%d' % self.nv; self.nv += 1; toks.append('>' + v); self.vecs[v] = lcls
        elif k < .60:
            toks += self.expr(lcls)[0:0] + [self.scalar(), r.choice(BIN)] + ['dup', '1', '&'] 
            v = 'c%d' % self.nv; self.nv += 1; toks.append('?' + v); self.vecs[v] = self.newcls()
        elif k < .72 and [v for v in self.vecs if v not in toks and '@' + v not in toks]:
            v = r.choice([v for v in self.vecs if v not in toks and '@' + v not in toks])
            idx = ['~1', 'drop'] 
            toks += ['dup', '7', '*', '0', 'max', '&' + v, '1', '-', 'min'] if r.random() < .985 else ["dup"]
            toks.append('!' + v)
        elif k < .80:
            s = 'S%d' % self.ns; self.ns += 1; toks.append(r.choice('$^') + s); self.scal.append(s)
        else: toks.append(r.choice(['out', 'out', 'out4']))
        self.lines.append(' '.join(toks))
    def program(self):
        for _ in range(self.r.randrange(1, 25)): self.line()
        # make everything observable
        for v in list(self.vecs)[-6:]: self.lines.append(v + ' out4')
        for s in self.scal[-6:]: self.lines.append('~1 ' + s + ' + out4')
        return ('\n'.join(self.lines) + '\n').encode()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler'); ap.add_argument('--n', type=int, default=300); ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, 'p')
    stats = {'ok': 0, 'fail99': 0, 'halt': 0, 'cerr': 0, 'runs': 0}
    for it in range(a.n):
        src = Gen(rnd).program()
        try:
            if a.compiler:
                p = subprocess.run([a.compiler], input=src, capture_output=True, preexec_fn=unlimit, timeout=20)
                try: strand.analyze(src); ref_rc = 0
                except strand.CompileError as e: ref_rc = e.code
                if p.returncode != ref_rc:
                    open('/tmp/fuzz_fail.strand', 'wb').write(src); print('COMPILE STATUS MISMATCH', p.returncode, ref_rc); sys.exit(1)
                if ref_rc: stats['cerr'] += 1; continue
                binary = p.stdout
            else:
                binary = strand.compile_(src)
        except strand.CompileError as e:
            stats['cerr'] += 1; continue
        with open(exe, 'wb') as f: f.write(binary)
        os.chmod(exe, 0o755)
        for n in rnd.sample(LENS, 6):
            inp = bytes(rnd.choice([rnd.randrange(256), rnd.randrange(4), 32, 65]) for _ in range(n))
            try: want = strand.interp(src, inp)
            except strand.TooBig: stats['toobig'] = stats.get('toobig', 0) + 1; continue
            p = subprocess.run([exe], input=inp, capture_output=True, preexec_fn=unlimit, timeout=20)
            got = (p.returncode, p.stdout)
            stats['runs'] += 1
            if got != want:
                open('/tmp/fuzz_fail.strand', 'wb').write(src); open('/tmp/fuzz_fail.in', 'wb').write(inp)
                print('MISMATCH iter', it, 'len', n, 'want', want[0], len(want[1]), 'got', got[0], len(got[1]), p.stderr[:200])
                print(src.decode()); sys.exit(1)
            stats['ok' if want[0] == 0 else 'fail99' if want[0] == 99 else 'halt'] += 1
    print('PASS', stats)

if __name__ == "__main__":
    main()
