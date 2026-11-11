#!/usr/bin/env python3
"""golex_gen.py > golex.strand -- a lexer for Go, in Strand.

Input: Go source. Output: the token stream go/scanner produces (mode 0: comments skipped, automatic
semicolons inserted), as little-endian dwords: every token's byte offset, then every token's kind
(go/token numbering). explore/golex is the reference; tests compare the two byte for byte.

How it works (every step is a line; nothing loops over tokens or bytes one at a time):
  1. context: an 8-state automaton says what each byte is inside of: code, a line comment, a block
     comment, a string, a raw string, a rune. Its input is the byte, the byte before it, and whether an
     odd run of backslashes precedes it (a 2-state automaton).
  2. words: an 8-state automaton over code bytes follows identifiers and numbers (decimal, hex, fraction,
     exponent), so `1e+5` is one token and `x+5` three.
  3. operators: a 4-state automaton does longest match over runs of operator characters: its state is
     how many characters of the current operator have been read; its input says whether the last two or
     three bytes form a longer operator.
  4. positions of token starts, token ends and newlines leave the byte lines as dword vectors.
  5. per token: kind from its first byte, its word state, its length; keywords by a perfect hash on the
     token's first twelve bytes; a newline becomes a semicolon when the token before it asks for one.
"""
import itertools, sys

KW = {'break': 61, 'case': 62, 'chan': 63, 'const': 64, 'continue': 65, 'default': 66, 'defer': 67, 'else': 68, 'fallthrough': 69, 'for': 70,
      'func': 71, 'go': 72, 'goto': 73, 'if': 74, 'import': 75, 'interface': 76, 'map': 77, 'package': 78, 'range': 79, 'return': 80,
      'select': 81, 'struct': 82, 'switch': 83, 'type': 84, 'var': 85}
OPS = {'+': 12, '-': 13, '*': 14, '/': 15, '%': 16, '&': 17, '|': 18, '^': 19, '<<': 20, '>>': 21, '&^': 22, '+=': 23, '-=': 24, '*=': 25,
       '/=': 26, '%=': 27, '&=': 28, '|=': 29, '^=': 30, '<<=': 31, '>>=': 32, '&^=': 33, '&&': 34, '||': 35, '<-': 36, '++': 37, '--': 38,
       '==': 39, '<': 40, '>': 41, '=': 42, '!': 43, '!=': 44, '<=': 45, '>=': 46, ':=': 47, '...': 48, '(': 49, '[': 50, '{': 51, ',': 52,
       '.': 53, ')': 54, ']': 55, '}': 56, ';': 57, ':': 58, '~': 88}
IDENT, INT, FLOAT, IMAG, CHAR, STRING, SEMI = 4, 5, 6, 7, 8, 9, 57
SEMI_AFTER = {IDENT, INT, FLOAT, IMAG, CHAR, STRING, 61, 65, 69, 80, 37, 38, 54, 55, 56}

def data(name, vals): return ' '.join(str(v) for v in vals) + ' =' + name
def tab256(f): return [f(c) for c in range(256)]
L = []

# ---- 1. context
# byte kinds: 0 other, 1 newline, 2 ", 3 ', 4 `, 5 /, 6 *, 7 backslash
gk = tab256(lambda c: {10: 1, 34: 2, 39: 3, 96: 4, 47: 5, 42: 6, 92: 7}.get(c, 0))
# classes for the context automaton, from (kind, kind of the byte before: 0 other / 1 slash / 2 star, escaped?)
#   0 other  1 newline  2 "  3 '  4 `  5 / after /  6 / after *  7 /  8 * after /  9 *
def mclass(k, pk, esc):
    if k == 1: return 1
    if k == 2: return 0 if esc else 2
    if k == 3: return 0 if esc else 3
    if k == 4: return 4
    if k == 5: return 5 if pk == 1 else 6 if pk == 2 else 7
    if k == 6: return 8 if pk == 1 else 9
    return 0
mcls = [mclass(i & 7, (i >> 3) & 3, i >> 5 & 1) if (i >> 3) & 3 < 3 and i < 64 else 0 for i in range(256)]
C, C1, LC, BO, BC, S, R, Q = range(8)       # code, code just after */, // comment, /* just opened, /* comment, "string", `raw`, 'rune'
def mstep(cl, s):
    if s in (C, C1):
        return {1: C, 2: S, 3: Q, 4: R}.get(cl, LC if cl == 5 and s == C else BO if cl == 8 and s == C else C)
    if s == LC: return C if cl == 1 else LC
    if s == BO: return BC
    if s == BC: return C1 if cl == 6 else BC
    if s == S: return C if cl in (1, 2) else S
    if s == R: return C if cl == 4 else R
    return C if cl in (1, 3) else Q
mdfa = [mstep(cl, s) for cl in range(10) for s in range(8)]

# ---- 2. words. classes: 0 not a word byte, 1 letter, 2 digit, 3 e, 4 p, 5 x, 6 '.', 7 '.' before a digit, 8 sign
def wclass(c):
    ch = chr(c)
    if ch in 'eE': return 3
    if ch in 'pP': return 4
    if ch in 'xX': return 5
    if ch.isalpha() and c < 128 or ch == '_' or c >= 128: return 1
    if ch.isdigit(): return 2
    if ch == '.': return 6
    if ch in '+-': return 8
    return 0
gw = tab256(wclass)
N0, ID, D, DF, XS, DX, H = range(7)         # none, identifier, decimal, fraction, after e/p (a sign may follow), exponent, hex
def wstep(cl, s):
    if s == 7: s = N0
    if cl == 0: return N0
    if cl == 1: return {N0: ID, ID: ID, D: D, DF: DF, XS: DX, DX: DX, H: H}[s]
    if cl == 2: return {N0: D, ID: ID, D: D, DF: DF, XS: DX, DX: DX, H: H}[s]
    if cl == 3: return {N0: ID, ID: ID, D: XS, DF: XS, XS: DX, DX: DX, H: H}[s]
    if cl == 4: return {N0: ID, ID: ID, D: D, DF: DF, XS: DX, DX: DX, H: XS}[s]
    if cl == 5: return {N0: ID, ID: ID, D: H, DF: DF, XS: DX, DX: DX, H: H}[s]
    if cl == 6: return {N0: N0, ID: N0, D: DF, DF: N0, XS: N0, DX: N0, H: H}[s]
    if cl == 7: return {N0: DF, ID: N0, D: DF, DF: N0, XS: N0, DX: N0, H: H}[s]
    return DX if s == XS else N0
wdfa = [wstep(cl, s) for cl in range(9) for s in range(8)]

# ---- 3. operators
OPCH = sorted(set(''.join(OPS)))
fcid = {ch: i + 1 for i, ch in enumerate(OPCH)}                       # 1..23
SC = '=+-<>&|^.'                                                      # second characters of two-character operators
gfc = tab256(lambda c: fcid.get(chr(c), 0))
gsc = tab256(lambda c: SC.index(chr(c)) + 1 if chr(c) in SC else 0)
gop = tab256(lambda c: 1 if chr(c) in OPCH else 0)
def pairval(a, b):                          # 0 not an operator; 1 an operator; 2 ".." (only as the start of "..."); 3 one that '=' extends
    if a + b == '..': return 2
    if a + b not in OPS: return 0
    return 3 if a + b + '=' in OPS else 1
e2t = [0] * 256
for a in OPCH:
    for j, b in enumerate(SC): e2t[(j + 1) * 24 + fcid[a]] = pairval(a, b)
# classes: 0 not an operator byte; else 1 + (extends a 1-char operator) + 2 * (extends a 2-char operator)
def ostep(cl, s):
    if cl == 0: return 0
    if cl - 1 & 1 and s == 1: return 2
    if cl - 1 & 2 and s == 2: return 3
    return 1
odfa = [ostep(cl, s) for cl in range(5) for s in range(4)]

# ---- 5. token kinds
def find(keys, size, cands):
    for p in cands:
        seen = {}
        if all(seen.setdefault(h(p, k) % size, k) == k for k in keys): return p
    raise SystemExit('no perfect hash')
M32 = 0xffffffff
def words(s):
    b = s.encode().ljust(12, b'\0'); return [int.from_bytes(b[4 * j:4 * j + 4], 'little') for j in range(3)]
def h(p, w): return (((w[0] * p[0] + w[1] * p[1] + w[2] * p[2]) & M32) >> p[3]) & 255
import random
_r = random.Random(7)
kwp = find([tuple(words(k)) for k in KW], 256, ((_r.randrange(1, 999999999, 2), _r.randrange(1, 999999999, 2), _r.randrange(1, 999999999, 2), _r.randrange(8, 25)) for _ in range(200000)))
kwk = [0] * 256
for k, kind in KW.items(): kwk[h(kwp, words(k))] = kind
def oh(p, k): return ((k[0] * p[0] + k[1] * p[1] + k[2] * p[2]) >> p[3]) & 255
h_save = h; h = oh
okeys = [(ord(o[0]), ord(o[1]) if len(o) > 1 else 0, len(o)) for o in OPS]
opp = find(okeys, 256, ((a, b, c, s) for s in (0, 3, 5, 7) for a in range(1, 60, 2) for b in range(1, 60, 2) for c in (1, 3, 7, 11, 19, 37, 61)))
opk = [0] * 256
for o, kind in OPS.items(): opk[oh(opp, (ord(o[0]), ord(o[1]) if len(o) > 1 else 0, len(o)))] = kind
semitab = [1 if k in SEMI_AFTER else 0 for k in range(128)]

# escape automaton: 0 plain, 1 a backslash that escapes the next byte, 2 an escaped byte
esc = [0, 2, 0, 0,  1, 2, 1, 0]
# from (kind, context after the byte): 0 nothing; 1 a byte of a string, raw string or rune (closing quote included);
# 3 the same and the token starts here unless the byte is escaped; 5 the same and the token starts here
def strv(k, m):
    if m == R and k == 4: return 5
    if m == S and k == 2 or m == Q and k == 3: return 3
    if m >= S: return 1
    return 1 if m == C and k in (2, 3, 4) else 0
strt = [strv(i & 7, i >> 3) if i < 64 else 0 for i in range(256)]
# what a token is, from its first byte: 0 operator, 1 identifier, 2 number, 3 string, 4 rune, 5 newline, 6 '.' (a number if a digit follows)
fam = tab256(lambda c: 5 if c == 10 else 3 if c in (34, 96) else 4 if c == 39 else 6 if c == 46 else 2 if 48 <= c <= 57 else 1 if gw[c] in (1, 3, 4, 5) else 0)
kww = [[0] * 256 for _ in range(3)]
for k, kind in KW.items():
    w = words(k); i = h_save(kwp, w)
    for j in range(3): kww[j][i] = w[j]

for n, v in (('gk', gk), ('mcls', mcls), ('mdfa', mdfa), ('esc', esc), ('strt', strt), ('gw', gw), ('wdfa', wdfa), ('gfc', gfc), ('gsc', gsc), ('gop', gop), ('e2t', e2t), ('odfa', odfa),
             ('fam', fam), ('kwk', kwk), ('kwa', kww[0]), ('kwb', kww[1]), ('kwc', kww[2]), ('opk', opk), ('semitab', semitab)):
    L.append(data(n, v))
L += f'''0 1 - >Neg
<text
&text >N
N 1 - >Tm
# One line over the bytes. Left behind: the positions where tokens start (ev) and end (te), and the word
# state of every byte (ws). A newline outside strings counts as a token here; most are dropped later.
#   k     kind of byte;  es  escape state (2: this byte is escaped)
#   m     context after the byte: 0 code, 1 code (just after */), 2 // comment, 3 /* (just opened), 4 /* comment,
#         5 string, 6 raw string, 7 rune
#   sv    string value of (k, m): see strt
#   code  the byte is program text: in code, not a closing quote, not the '/' that opens a comment
#   ws    word state after the byte: 0 none, 1 identifier, 2 decimal, 3 fraction, 4 after e/p, 5 exponent, 6 hex
#   t,pt  what (previous byte, this byte) and (the two before) are as operators: 0 nothing, 1 an operator,
#         2 "..", 3 an operator that '=' extends
#   os    operator state: how many bytes of the current operator have been read
#   tc    token byte;  ts  first byte of a token
bytes :c @gk :k back @gk :bk 5 = bk 6 = 2 * + 8 * + 0 k 7 = /esc :es 2 = 32 * + + @mcls \\mdfa :m 8 * k + @strt :sv m 0 = sv 0 = & c 47 = peek 47 = peek 42 = | & 1 ^ & :code c @gw c 46 = peek @gw 2 = & + * \\wdfa :ws 0 over prev :pws drop drop 0 != pws 0 = & :wst drop back @gfc c @gsc 24 * + @e2t :t dup 1 = swap 3 = | t 2 = peek 46 = & | back2 @gfc back @gsc 24 * + @e2t :pt 3 = c 61 = & pt 2 = c 46 = & | 2 * + 1 + c @gop code * ws 0 = * * :oc 0 0 oc /odfa :os drop drop drop drop k 1 = m 6 != & :nl ws 0 != | os 0 != | sv 0 != | :tc wst os 1 = | nl | sv 5 = | sv 3 = es 2 != & | :ts %ev 0 0 0 tc prev :ptc drop drop drop drop 1 ^ ts | ptc & %te end
&ev 1 - >Em
&te >Te
Te 1 - >Tem
# One line over the tokens: kind from the first bytes (w0), the length, the word state at the end; keywords
# by a perfect hash of the first twelve bytes. kc = kind, plus 256 if a newline after this token is a semicolon.
ev :s idx Te < idx Tem min 0 max @te N sel :e over - :len drop dup {{text :w0 255 & :c0 @fam :f0 6 = w0 8 >> 255 & :c1 @fam 2 = & f0 2 = | :isnum drop e 1 - :e1 [text 105 = {IMAG} e1 [ws :we 2 = we 6 = | {INT} {FLOAT} sel sel :knum drop w0 Neg 4 len 4 min - 8 * >> & :a s 4 + Tm min {{text Neg 4 len 4 - 0 max 4 min - 8 * >> & :b s 8 + Tm min {{text Neg 4 len 8 - 0 max 4 min - 8 * >> & :d drop drop drop a {kwp[0]} * b {kwp[1]} * + d {kwp[2]} * + {kwp[3]} >> 255 & :hk @kwa a = hk @kwb b = & hk @kwc d = & len 12 <= & hk @kwk * :kk 0 = {IDENT} * kk + :kid drop c1 len 1 > * {opp[1]} * c0 {opp[0]} * + len {opp[2]} * + {opp[3]} >> 255 & @opk :kop drop f0 1 = kid kop sel :x1 drop isnum knum x1 sel :x2 drop f0 4 = {CHAR} x2 sel :x3 drop f0 3 = {STRING} x3 sel :x4 drop f0 5 = 0 x4 sel :kd @semitab 256 * kd + swap drop >kc
# a newline is a semicolon if the token before it asks for one
kc 255 > idx Em = & $Ls
ev kc 255 & 0 != kc prev 255 > | outif4
~1 0 * N + Ls outif4
kc 255 & :kk2 0 = {SEMI} * kk2 + kk2 0 != kc prev 255 > | outif4
~1 0 * {SEMI} + Ls outif4
'''.split('\n')
sys.stdout.write('\n'.join(L))
