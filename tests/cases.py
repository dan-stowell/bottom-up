#!/usr/bin/env python3
"""Hand-written cases: one per compile error, plus edge programs. Checks the reference's status is
what the spec says AND that the compiler under test agrees byte for byte.   cases.py COMPILER"""
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
from spec import *
CASES = [
 (b'', 0), (b'\n', 0), (b'# only a comment', 0), (b'input out\n', 0), (b'input out', 0),
 (b'1 2 3 "abc" =d\nd out\n', 0), (b'=d\n', 0), (b'"" =d\n', 0), (b'"a # b" =d\n', E_QUOTE),
 (b'input >x # trailing "comment" with quotes\nx out\n', 0),
 (b'"abc =d\n', E_QUOTE), (b'1 a"b" =d\n', E_QUOTE), (b'"a"b =d\n', E_QUOTE), (b'"a""b" =d\n', E_QUOTE),
 (b'12a >A\n', E_LEX), (b'1234567890 >A\n', E_LEX), (b'999999999 >A\n', 0), (b'~1x out\n', E_LEX),
 (b'1 >' + b'A' * 64 + b'\n', 0), (b'1 >' + b'A' * 65 + b'\n', E_NAMELEN), (b'input >' + b'x' * 65 + b'\n', E_NAMELEN),
 (b'input %% out\n', E_TOKEN), (b'input >scan\n', E_TOKEN), (b'input @Out out\n', E_TOKEN), (b'1 >a\n', 0), (b'1 $a\n', E_TOKEN),
 (b'input "x" & out\n', E_DATA), (b'1 input =d\n', E_DATA), (b'1 =d 2\n', E_DATA), (b'1 >A =d\n', E_DATA), (b'~3 4 =d\n', E_DATA),
 (b'input input out\n', E_DEPTH), (b'input\n', E_DEPTH), (b'input out out\n', E_DEPTH), (b'input 1 2 3 4 5 6 7 8 + + + + + + + + out\n', E_DEPTH),
 (b'input 1 2 3 4 5 6 7 + + + + + + + out\n', 0), (b'input out input out\n', E_DEPTH), (b'+ out\n', E_DEPTH),
 (b'input drop\n', E_STENCIL), (b'input 1 ~3 + + out\n', E_STENCIL), (b'out\n', E_DEPTH), (b'input dup input + + out\n', E_STENCIL),
 (b'input scan scan out\n', E_CARRY), (b'input scan prev out\n', E_CARRY), (b'input scan scanmax out\n', 0), (b'input scan dup prev + out\n', 0),
 (b'input >x\nx x !x\n', E_ALIAS), (b'input >x\n~3 dup @x swap !x\n', E_ALIAS), (b'input >x\n~3 &x + dup !x\n', 0),
 (b'x out\n', E_UNDEF), (b'~N out\n', E_UNDEF), (b'input @x out\n', E_UNDEF), (b'input &x + out\n', E_UNDEF), (b'~3 dup !x\n', E_UNDEF),
 (b'x out\ninput >x\n', 0),
 (b'input :a 1 + :b end\na b + out\n', 0), (b'input :a a + out\n', 0), (b'input :a &a + out\n', 0),
 (b'input >a\na :a end\n', E_TEE), (b'input :a end\ninput >a\n', E_TEE), (b'input :a :a end\n', E_TEE),
 (b'input >b\nb a + :a end\n', E_TEE), (b'input :a @a end\n', E_TEE), (b'input dup 1 & %k 5 end\n', E_DEPTH),
 (b'input 7 over 1 & %k end\nk out\n', 0), (b'input 7 over 1 & %k k + end\n', E_TEE), (b'input :a dup !a\n', E_TEE),
 (b'input end\n', 0), (b'input 1 :k + end\n', 0), (b'1 :k >A\nk out\n', 0),
 (b"input\xff\x80 out\n", E_UNDEF), (b"\xff out\n", E_TOKEN), (b'\x01\x02input out\n', 0),
]
def main():
    comp = sys.argv[1]
    for src, code in CASES:
        try: want = (0, strand.compile_(src))
        except strand.CompileError as e: want = (e.code, b'')
        assert want[0] == code, ('reference disagrees with spec table', src, want[0], code)
        p = subprocess.run([comp], input=src, capture_output=True, timeout=20)
        assert (p.returncode, p.stdout) == want, ('compiler disagrees with reference', src, p.returncode, want[0], p.stderr)
    print('PASS', comp, len(CASES), 'cases')
main()
