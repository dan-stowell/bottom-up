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
LENS = [0, 1, 2, 3, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64, 65, 79, 80, 81, 127, 128, 129, 255, 256, 257, 1000, 4095, 4096, 4097, 20001]

class Gen:
    def __init__(self, rnd):
        self.r = rnd; self.vecs = {}; self.scal = []; self.lines = []; self.nv = 0; self.ns = 0; self.cls = 0; self.teenames = set()
    def newcls(self): self.cls += 1; return self.cls
    def lit(self):
        r = self.r
        return str(r.choice([0, 1, 2, 3, 7, 15, 16, 17, 31, 32, 33, 64, 255, 256, 65535, 999999999, r.randrange(1000)]))
    def scalar(self):
        return self.r.choice(self.scal) if self.scal and self.r.random() < .5 else self.lit()
    def expr(self, lcls, depth_budget=7):
        """tokens after the lead; stack starts at depth 1; ends at depth 1 (or 2 if want2)."""
        r = self.r; toks = []; d = 1; used = set(); teed = []
        same = [v for v, c in self.vecs.items() if c == lcls]
        for _ in range(r.randrange(0, 12)):
            ch = r.random()
            if ch < .05:                         # tee store: value stays on the stack
                v = 't%d' % self.nv; self.nv += 1; toks.append(':' + v); self.pending.append((v, lcls)); teed.append(v)
            elif ch < .08 and d >= 2 and d < depth_budget:   # tee compress, mid-line
                v = 'k%d' % self.nv; self.nv += 1; toks += ['dup', '1', '&', '%' + v]; d -= 1; self.pending.append((v, None))
            elif ch < .11 and teed and d < depth_budget:      # re-read a tee'd vector later on its own line
                toks.append(r.choice(teed) if r.random() < .8 else '&' + r.choice(teed)); d += 1
            elif ch < .25 and d < depth_budget:
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
            elif ch < .83 and getattr(self, 'bvecs', None) and d < depth_budget - 2:     # read a byte vector by index
                v = r.choice(self.bvecs); g = r.choice('[{')
                if r.random() < .97: toks += ["0", "max", "&" + v, "1", "-", "min", g + v]   # clamped (still fails on an empty vector)
                else: toks.append(g + v)
            elif ch < .85 and d < depth_budget: toks.append(r.choice(['dup', 'over', 'idx'] if d >= 2 else ['dup', 'idx'])); d += 1
            elif ch < .88 and d >= 2: toks.append('swap')
            elif ch < .90 and d >= 2: toks.append('drop'); d -= 1
            elif self.vecs and d < depth_budget - 2:
                v = r.choice(list(self.vecs))
                if r.random() < .985: toks += ["0", "max", "&" + v, "1", "-", "min", "@" + v]   # clamped
                else: toks.append('@' + v)                                                 # may fail: fine
        while d > 1: toks.append(r.choice(BIN)); d -= 1
        return toks
    def byteline(self):
        """a streaming byte line: 64 lanes, byte arithmetic, table lookups, local tees, streaming sinks"""
        r = self.r; toks = ['bytes']; d = 1; used = set(); teed = []; newvecs = []
        if not hasattr(self, 'bvecs'): self.bvecs = []
        if self.bvecs and r.random() < .35: toks = ["'" + r.choice(self.bvecs)]
        BOPS = ['+', '-', '*', '&', '|', '^', 'min', 'max', '<', '<=', '>', '>=', '=', '!=']
        for _ in range(r.randrange(0, 14)):
            ch = r.random()
            if ch < .25 and d < 7:
                toks.append(r.choice(self.scal) if self.scal and r.random() < .3 else r.choice(['0', '1', '10', '32', '64', '97', '122', '127', '128', '255', '256', '300', str(r.randrange(256))])); d += 1
            elif ch < .55 and d >= 2: toks.append(r.choice(BOPS)); d -= 1
            elif ch < .62 and d >= 3: toks.append('sel'); d -= 2
            elif ch < .72 and ('p', d) not in used: used.add(('p', d)); toks.append('prev')
            elif ch < .76 and ('p', d) not in used: used.add(('p', d)); toks.append('/' + self.automaton())
            elif ch < .79 and ('p', d) not in used: used.add(('p', d)); toks.append('\\' + self.automaton(8))
            elif ch < .80 and d < 7: toks.append('peek'); d += 1
            elif ch < .805 and d < 7 and self.bvecs: toks.append(r.choice(self.bvecs)); d += 1      # a stored byte vector (fails unless its length is the line's)
            elif ch < .82 and d < 7: toks.append(r.choice(['dup', 'over'] if d >= 2 else ['dup'])); d += 1
            elif ch < .84 and d >= 2: toks.append(r.choice(['swap', 'drop'])); d -= toks[-1] == 'drop'
            elif ch < .90:
                v = 'l%d' % self.nv; self.nv += 1; toks.append(':' + v); teed.append(v)   # chunk-local by construction
            elif ch < .93 and teed and d < 7: toks.append(r.choice(teed)); d += 1
            elif ch < .95 and d >= 2:              # positions of the nonzero lanes, as a dword vector
                v = 'w%d' % self.nv; self.nv += 1; toks.append('%' + v); d -= 1; newvecs.append(v)
            elif ch < .97 and d < 7:               # a byte tee that outlives the line: a byte vector
                v = 'b%d' % self.nv; self.nv += 1; toks.append(':' + v); self.bvecs.append(v)
            else: toks.append('@' + self.table256())
        while d > 2: toks.append(r.choice(BOPS)); d -= 1
        k = r.random()
        if d == 2:
            if k < .5: toks += ['dup', '1', '&'][0:0] + ['outif']
            else: toks += [r.choice(BOPS), r.choice(['out', 'end'])]
        elif k < .40: toks.append('out')
        elif k < .50:
            v = 'b%d' % self.nv; self.nv += 1; toks.append('>' + v); self.bvecs.append(v)
        elif k < .9:
            sn = 'S%d' % self.ns; self.ns += 1; toks.append('$' + sn); self.scal.append(sn)
        else: toks.append('end')
        self.lines.append(' '.join(toks))
        for v in newvecs: self.vecs[v] = self.newcls()
    def automaton(self, states=4):
        if states == 8:
            if not hasattr(self, 'autos8'): self.autos8 = []
            if not self.autos8 or self.r.random() < .4:
                v = 'ax%d' % self.nv; self.nv += 1
                n = self.r.choice([128, 128, 128, 40, 0, 1, 127, 129, 200, 64, 65])
                big = self.r.random() < .3
                self.lines.append(' '.join(str(self.r.randrange(1000 if big else 8)) for _ in range(n)) + ' =' + v); self.autos8.append(v)
            return self.r.choice(self.autos8)
        if not hasattr(self, 'autos'): self.autos = []
        if not self.autos or self.r.random() < .4:
            v = 'au%d' % self.nv; self.nv += 1
            n = self.r.choice([64, 64, 64, 20, 0, 1, 63, 65, 100])       # short tables: missing entries are 0
            big = self.r.random() < .3                                  # entries are taken & 3
            self.lines.append(' '.join(str(self.r.randrange(1000 if big else 4)) for _ in range(n)) + ' =' + v); self.autos.append(v)
        return self.r.choice(self.autos)
    def table256(self):
        if not hasattr(self, 'tabs'): self.tabs = []
        if not self.tabs or self.r.random() < .3:
            v = 'tb%d' % self.nv; self.nv += 1
            n = self.r.choice([256, 256, 256, 300, 255])            # 255: the lookup must fail at run time
            self.lines.append(' '.join(str(self.r.randrange(1000)) for _ in range(n)) + ' =' + v); self.tabs.append(v)
        return self.r.choice(self.tabs)
    def line(self):
        if self.r.random() < .03:                 # the input as a byte vector, or a vector of zero bytes
            if not hasattr(self, 'bvecs'): self.bvecs = []
            v = 'b%d' % self.nv; self.nv += 1; self.bvecs.append(v)
            self.lines.append('<' + v if self.r.random() < .5 else self.r.choice(['0', '1', '63', '64', '65', '1000', '5000']) + ' ,' + v); return
        if self.r.random() < .12: return self.byteline()
        self.pending = []
        self._line()
        for v, c in self.pending: self.vecs[v] = self.newcls() if c is None or c == -1 else c; self.teenames.add(v)
    def _line(self):
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
        elif lead < .68: toks = ['input']; lcls = 0
        elif lead < .75: toks = ['input4']; lcls = -4
        else:
            s = r.choice(self.scal) if self.scal and r.random() < .4 else r.choice(['0', '1', '15', '16', '17', '33', '100'])
            toks = ['~' + s]; lcls = ('iota', s) if s[0] != 'S' else self.newcls()
        toks += self.expr(lcls)
        k = r.random()
        if k < .45:
            olds = [v for v in self.vecs if v[0] in 'vdc' and v not in self.teenames]
            if olds and r.random() < .35: v = r.choice(olds)          # overwrite an existing vector (maybe one this line reads)
            else: v = 'v%d' % self.nv; self.nv += 1
            toks.append('>' + v); self.vecs[v] = lcls
        elif k < .60:
            toks += [self.scalar(), r.choice(BIN)] + ["dup", "1", "&"]
            v = 'c%d' % self.nv; self.nv += 1; toks.append('?' + v); self.vecs[v] = self.newcls()
        elif k < .72 and [v for v in self.vecs if v not in toks and '@' + v not in toks]:
            v = r.choice([v for v in self.vecs if v not in toks and '@' + v not in toks])
            idx = ['~1', 'drop'] 
            toks += ['dup', '7', '*', r.choice(['0', '0', '3']), r.choice(['max', 'max', '-']), '&' + v, '1', '-', 'min'] if r.random() < .985 else ["dup"]
            toks.append('!' + v)
        elif k < .78:
            s = 'S%d' % self.ns; self.ns += 1; toks.append(r.choice('$^') + s); self.scal.append(s)
        elif k < .82:                               # the low bytes, as a byte vector
            if not hasattr(self, 'bvecs'): self.bvecs = []
            v = 'b%d' % self.nv; self.nv += 1; toks.append('.' + v); self.bvecs.append(v)
        elif k < .85 and len(getattr(self, 'bvecs', [])) >= 2:     # slices from one byte vector into another
            src, dst = r.sample(self.bvecs, 2)
            sl = lambda v, ln: ['dup', r.choice(['3', '5', '64', '129']), '*', '0', 'max', '&' + v, ln, '-', '0', 'max', 'min']
            ln = r.choice(['0', '1', '3', '17', '64', '65', '200']) if r.random() < .9 else '100000'
            toks += sl(src, ln) + [ln] + ['over'] + sl(dst, ln)[1:] + ['(' + src, ')' + dst, 'end']
        elif k < .88 and getattr(self, 'bvecs', None):              # a dword at a byte offset
            v = r.choice(self.bvecs)
            toks += ['dup', '7', '*', r.choice(['0', '0', '3']), r.choice(['max', 'max', '-']), '&' + v, '4', '-', 'min'] if r.random() < .97 else ['dup']
            toks.append('}' + v)
        elif k < .92: toks.append(r.choice(['out', 'out', 'out4', 'end']))
        else: toks += [self.scalar(), r.choice(BIN), 'dup', '1', '&', r.choice(['outif', 'outif4'])]
        self.lines.append(' '.join(toks))
    def program(self):
        for _ in range(self.r.choice([self.r.randrange(1, 25)] * 9 + [self.r.randrange(60, 200)])): self.line()
        # make everything observable
        for v in list(self.vecs)[-6:]: self.lines.append(v + ' out4')
        for v in getattr(self, 'bvecs', [])[-4:]: self.lines.append("'" + v + ' out')
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
            # run twice: normally, and in "eager" mode, where every splittable line of 64+ elements
            # big enough to split is forked across all cores, in small blocks
            for mode in ([exe], [exe, 'eager']):
                if len(mode) == 1:        # normal mode reads a regular file (the mmap path); eager mode a pipe
                    open(exe + '.in', 'wb').write(inp)
                    p = subprocess.run(mode, stdin=open(exe + '.in', 'rb'), capture_output=True, preexec_fn=unlimit, timeout=20)
                else:
                    p = subprocess.run(mode, input=inp, capture_output=True, preexec_fn=unlimit, timeout=20)
                got = (p.returncode, p.stdout if p.returncode == 0 else b'')   # output is streamed: unspecified on failure
                if got != want: break
            stats['runs'] += 1
            if got != want:
                open('/tmp/fuzz_fail.strand', 'wb').write(src); open('/tmp/fuzz_fail.in', 'wb').write(inp)
                print('MISMATCH iter', it, 'mode', mode[1:], 'len', n, 'want', want[0], len(want[1]), 'got', got[0], len(got[1]), p.stderr[:200])
                print(src.decode()); sys.exit(1)
            stats['ok' if want[0] == 0 else 'fail99' if want[0] == 99 else 'halt'] += 1
    print('PASS', stats)

if __name__ == "__main__":
    main()
