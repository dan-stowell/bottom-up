#!/usr/bin/env python3
"""Hand-written cases: one per compile error, plus edge programs. Checks the reference's status is
what the spec says AND that the compiler under test agrees byte for byte.   cases.py COMPILER"""
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
from spec import *
def race_witness():
    """A program that loses the four-round name race: a tree of 16 names in which every unresolved name
    has a later blocker in each round. Searched for, so that it follows the fingerprint definition."""
    import random
    mask = race_mask(32); cands = [b'X%d' % i for i in range(1, 60000)]; H = {c: name_hash(c) for c in cands}
    bykey = [{} for _ in range(ROUNDS)]
    for c in cands:
        for r in range(ROUNDS): bykey[r].setdefault(race_key(r, H[c], mask), []).append(c)
    rnd = random.Random(1)
    def build(n, r, used):
        out = [n]
        for i in range(r):
            b = rnd.choice([c for c in bykey[i][race_key(i, H[n], mask)] if c not in used]); used.add(b); out += build(b, i, used)
        return out
    for _ in range(5000):
        t = rnd.choice(cands)
        try: names = build(t, ROUNDS, {t})
        except IndexError: continue
        src = b''.join(b'0 >%s\n' % n for n in names)
        try: strand.analyze(src)
        except strand.CompileError as e:
            if e.code == E_RACE: return src
    raise SystemExit('no race witness found')
CASES = [
 (b'', 0), (b'\n', 0), (b'# only a comment', 0), (b'input out\n', 0), (b'input out', 0),
 (b'1 2 3 "abc" =d\nd out\n', 0), (b'=d\n', 0), (b'"" =d\n', 0), (b'"a # b" =d\n', E_QUOTE),
 (b'input >x # trailing "comment" with quotes\nx out\n', 0),
 (b'"abc =d\n', E_QUOTE), (b'1 a"b" =d\n', E_QUOTE), (b'"a"b =d\n', E_QUOTE), (b'"a""b" =d\n', E_QUOTE),
 (b'12a >A\n', E_LEX), (b'1234567890 >A\n', 0), (b'12345678901 >A\n', E_LEX), (b'9999999999 >A\n', 0), (b'4294967295 >A\n', 0), (b'999999999 >A\n', 0), (b'~1x out\n', E_LEX),
 (b'1 >' + b'A' * 16 + b'\n', 0), (b'1 >' + b'A' * 17 + b'\n', E_NAMELEN), (b'input >' + b'x' * 17 + b'\n', E_NAMELEN),
 (b'input %% out\n', E_TOKEN), (b'input >scan\n', E_TOKEN), (b'input @Out out\n', E_TOKEN), (b'1 >a\n', 0), (b'1 $a\n', E_TOKEN),
 (b'input "x" & out\n', E_DATA), (b'1 input =d\n', E_DATA), (b'1 =d 2\n', E_DATA), (b'1 >A =d\n', E_DATA), (b'~3 4 =d\n', E_DATA),
 (b'input input out\n', E_DEPTH), (b'input\n', E_DEPTH), (b'input out out\n', E_DEPTH), (b'input 1 2 3 4 5 6 7 8 + + + + + + + + out\n', E_DEPTH),
 (b'input 1 2 3 4 5 6 7 + + + + + + + out\n', 0), (b'input out input out\n', E_DEPTH), (b'+ out\n', E_DEPTH),
 (b'input drop\n', E_STENCIL), (b'input 1 ~3 + + out\n', E_STENCIL), (b'out\n', E_DEPTH), (b'input dup input + + out\n', E_STENCIL),
 (b'input scan scan out\n', E_CARRY), (b'input scan prev out\n', E_CARRY), (b'input scan scanmax out\n', 0), (b'input scan dup prev + out\n', 0),
 (b'input >x\nx x !x\n', E_ALIAS), (b'input >x\n~3 dup @x swap !x\n', E_ALIAS), (b'input >x\n~3 &x + dup !x\n', 0),
 (b'x out\n', E_UNDEF), (b'~N out\n', E_UNDEF), (b'input @x out\n', E_UNDEF), (b'input &x + out\n', E_UNDEF), (b'~3 dup !x\n', E_UNDEF),
 (b'x out\ninput >x\n', 0),
 (b'bytes :t dup 32 > %p end\np [t out\np {t out4\n', 0), (b'bytes >t\nt out\n', E_TYPE), (b'input >t\n~3 [t out\n', E_TYPE), (b'bytes >t\n~3 @t out\n', E_TYPE),
 (b'bytes >t\ninput >t\n', E_TYPE), (b'bytes dup %p end\n~2 {p out\n', E_TYPE), (b'bytes %p\n', E_STENCIL), (b'bytes dup %p %p end\n', E_DEPTH), (b'bytes dup dup %p %p end\n', E_TEE),
 (b'bytes dup %p end\np out4\n&p >N\nN out4\n', 0), (b'bytes >t\n&t >N\nN out4\n~1 [t out\n', 0), (b'input [t out\n', E_UNDEF),
 (b"bytes >s\n~40 0 * 45 + .d\n~3 dup 2 * over 1 + idx 5 * (s )d end\n'd out\n~2 dup 9 * 65 + swap 20 * 12 + }d\n'd out\n", 0),
 (b"input .t\nt out\n", E_TYPE), (b"input >t\n't out\n", E_TYPE), (b"input .t\n~2 dup }t\n~2 {t out4\n't 1 + out\n", 0), (b"input .t\n~2 {t dup }t\n", E_ALIAS),
 (b"input .t\n~1 dup dup dup )t end\n", 0), (b"input .t\ninput .u\n~1 dup dup dup (t (u )u end\n", E_ALIAS), (b"input .t\ninput .u\n~1 dup dup dup (t )u dup dup dup )t end\n", E_ALIAS), (b"input .t\ninput .u\n~1 0 * {u dup dup dup (t )u end\n", E_ALIAS), (b"input .t\ninput .u\n~1 dup dup dup (t )u dup dup dup )u end\n", 0), (b"input .t\ninput 't + out\n", E_STENCIL), (b"'t out\n", E_UNDEF),
 (b"<r\n~2 {r out4\n'r out\n", 0), (b"<r\nr out\n", E_TYPE), (b"<r 1 + out\n", E_DEPTH), (b"input <r out\n", E_STENCIL), (b"5 ,z\n'z out\n~1 dup }z\n", 0), (b"input ,z\n", 0), (b",z\n", E_DEPTH),
 (b'input :a 1 + :b end\na b + out\n', 0), (b'input :a a + out\n', 0), (b'input :a &a + out\n', 0),
 (b'input >a\na :a end\n', E_TEE), (b'input :a end\ninput >a\n', E_TEE), (b'input :a :a end\n', E_TEE),
 (b'input >b\nb a + :a end\n', E_TEE), (b'input :a @a end\n', E_TEE), (b'input dup 1 & %k 5 end\n', E_DEPTH),
 (b'input 7 over 1 & %k end\nk out\n', 0), (b'input 7 over 1 & %k k + end\n', E_TEE), (b'input dup 1 & %k\n', E_STENCIL), (b'input idx + out\n', 0), (b'idx out\n', E_STENCIL), (b'input :a dup !a\n', E_TEE),
 (b'input end\n', 0),
 # byte lines
 (b'bytes 10 = $N\n~1 N + out4\n', 0), (b'bytes dup prev - out\n', 0), (b'bytes :c 97 >= c 122 <= & c 32 - c sel out\n', 0),
 (b'bytes dup 64 > outif\n', 0), (b'bytes scan out\n', E_STENCIL), (b'bytes >x\n', 0), (b'input >x\nbytes x + out\n', E_TYPE),
 (b'bytes :c end\nc out\n', E_TYPE), (b'bytes idx + out\n', E_STENCIL), (b'input bytes + out\n', E_STENCIL), (b'bytes 3 * 300 + out\n', 0),
 (b'~256 255 swap - >t\nbytes @t out\n', 0), (b'~255 >t\nbytes @t out\n', 0),
 # automata
 (b'0 1 2 2 1 1 2 2 2 1 0 2 0 0 2 2 0 1 3 2 =d\nbytes 7 & /d 48 + out\n', 0), (b'=d\nbytes /d 65 + out\n', 0), (b'3 =d\nbytes 0 & /d out\n', 0),
 (b'1 2 3 0 =d\nbytes 0 & /d 48 + out\n', 0), (b'1 2 3 0 =d\nbytes 0 & /d dup prev + $S\n~1 S + out4\n', 0),
 (b'1 2 3 0 =d\ninput /d out\n', E_STENCIL), (b'1 2 3 0 =d\nbytes /d /d out\n', E_CARRY), (b'1 2 3 0 =d\nbytes prev /d out\n', E_CARRY),
 (b'1 2 3 0 =d\nbytes /d dup /d + out\n', 0), (b'bytes /x out\n', E_UNDEF), (b'bytes /D out\n', E_TOKEN), (b'1 2 =d\n/d out\n', E_DEPTH),
 (b'1 2 3 0 0 0 0 0 0 1 2 3 =d\nbytes 64 > /d :s 1 = s prev 2 = & $C\n~1 C + out4\n', 0),
 # release-on-overwrite, self-reassignment, replacing static data, scatter into static data
 (b'input >x\nx 1 + >x\nx 1 + >x\nx out\n', 0), (b'1 2 3 =d\nd 1 + >d\nd out4\n', 0), (b'1 2 3 =d\nd idx !d\nd out4\n', E_ALIAS),
 (b'1 2 3 =d\n~3 9 + idx !d\nd out4\n', 0), (b'input >a\na >b\na >c\nb >a\nc >b\na b + out\n', 0),
 # the two programs Codex found: a 32-bit hash collision under the old hash, and many distinct names
 (b'1 >xfntrxrmgiq\n2 >xqhhsigtfyu\n', 0), (b''.join(b'1 >X%d\n' % i for i in range(3000)), 0), (b'input 1 :k + end\n', 0), (b'1 :k >A\nk out\n', 0),
 (b"input\xff\x80 out\n", E_UNDEF), (b"\xff out\n", E_TOKEN), (b'\x01\x02input out\n', 0),
 (None, E_RACE),      # replaced below by a generated witness
]
CASES = [(race_witness() if src is None else src, code) for src, code in CASES]
def main():
    comp = sys.argv[1]
    for src, code in CASES:
        try: want = (0, strand.compile_(src))
        except strand.CompileError as e: want = (e.code, b'')
        assert want[0] == code, ('reference disagrees with spec table', src, want[0], code)
        p = subprocess.run([comp], input=src, capture_output=True, timeout=20)
        assert (p.returncode, p.stdout) == want, ('compiler disagrees with reference', src[:200], p.returncode, want[0], p.stderr)
        if want[0] == 0 and len(src) < 500:      # the compiled program must also behave like the interpreter
            open('/tmp/case_bin', 'wb').write(want[1]); os.chmod('/tmp/case_bin', 0o755)
            for inp in (b'', b'a', b'Hello, world: 0123456789 abcdef', bytes(range(256)) * 3):
                for mode in (['/tmp/case_bin'], ['/tmp/case_bin', 'eager']):
                    r = subprocess.run(mode, input=inp, capture_output=True, timeout=20)
                    assert (r.returncode, r.stdout if r.returncode == 0 else b'') == strand.interp(src, inp), ('binary disagrees with interpreter', src, len(inp), mode)
    print('PASS', comp, len(CASES), 'cases')
main()
