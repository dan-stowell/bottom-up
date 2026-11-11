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
# index: 4 * kind + 32 * (kind of the byte before) + 2 * escaped. Tables hand out their values already
# scaled (gk4, gpk, gsc24): a byte multiply is eight instructions, an addition one.
mcls = [mclass((i >> 2) & 7, i >> 5, (i >> 1) & 1) if i >> 5 < 3 and not i & 1 else 0 for i in range(256)]
gk4 = [4 * x for x in gk]
gpk = tab256(lambda c: 32 if c == 47 else 64 if c == 42 else 0)
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

# ---- 2. words: identifiers and numbers, as go/scanner's scanNumber delimits them on input it accepts.
# States: N0 not in a word; N1, N2 the same, one or two dots of a possible `...` just read; ID identifier; D digits
# (integer part, or after 0b / 0o); DF fraction; H hex mantissa; DX exponent digits, or just after the suffix i.
N0, N1, N2, ID, D, DF, H, DX = range(8)
# What a byte is by itself:
#   0 none  1 letter  2 digit  3 _  4 hex letter (a c d f)  5 e  6 p  7 x  8 o  9 b  10 i  11 .  12 sign
def wcat(c):
    ch = chr(c)
    if c >= 128: return 1
    if ch == 'i': return 10
    if ch in 'eE': return 5
    if ch in 'pP': return 6
    if ch in 'xX': return 7
    if ch in 'oO': return 8
    if ch in 'bB': return 9
    if ch in 'acdfACDF': return 4
    if ch.isalpha(): return 1
    if ch == '_': return 3
    if ch.isdigit(): return 2
    if ch == '.': return 11
    if ch in '+-': return 12
    return 0
gcat = tab256(wcat)
# ... and what its neighbours make of it. The class the automaton reads comes from one table, indexed by
#   category + 16 * (the byte before: 1 '0', 2 e or p, 3 i) + 64 * (the byte two before could not continue a number
#   that the '0' before would be part of) + 128 * (the next byte is a digit)
# Classes: 0 none 1 letter 2 digit 3 _ 4 hex letter 5 e 6 p 7 x after a lone 0 8 o/b after a lone 0 9 i 10 . 11 . before a digit
# 12 sign after e/p 13 digit after i. After an i everything is a letter or class 13: a number ends with its suffix i,
# and DX is the state after it, so `1ii` and `1i1` are two tokens each without a state for "after the suffix".
gb1 = tab256(lambda c: 16 if c == 48 else 32 if chr(c) in 'eEpP' else 48 if c == 105 else 0)
glo = tab256(lambda c: 0 if c >= 128 or chr(c) in '0123456789_abcdefABCDEFoOxX' else 64)     # in state D, "0" is the whole number so far
def wclass2(cat, b1, lone, nd):
    if cat in (1, 3, 4, 5, 6, 10) and b1 == 3: return 1
    if cat == 2: return 13 if b1 == 3 else 2
    if cat == 7: return 7 if b1 == 1 and lone else 1
    if cat == 8: return 8 if b1 == 1 and lone else 1
    if cat == 9: return 8 if b1 == 1 and lone else 4
    if cat == 10: return 9
    if cat == 11: return 11 if nd else 10
    if cat == 12: return 12 if b1 == 2 else 0
    return cat
wcls = [wclass2(i & 15, (i >> 4) & 3, (i >> 6) & 1, i >> 7) if i & 15 < 13 else 0 for i in range(256)]
def wstep(cl, s):
    nw = s in (N0, N1, N2)
    if cl == 0: return N0
    if cl == 1: return ID
    if cl in (2, 3): return ID if cl == 3 and nw else D if nw else s
    if cl == 4: return H if s == H else ID
    if cl == 5: return DX if s in (D, DF) else H if s == H else ID
    if cl == 6: return DX if s == H else ID
    if cl == 7: return H if s == D else ID
    if cl == 8: return s if s in (D, H) else ID   # (in H only after `.0`, where b is a digit; o there is an error)
    if cl == 9: return ID if nw or s == ID else DX
    if cl == 10: return {N0: N1, N1: N2, N2: N0, ID: N1, D: DF, DF: N1, H: H, DX: N1}[s]
    if cl == 11: return {N0: DF, N1: DF, N2: N0, ID: DF, D: DF, DF: DF, H: H, DX: DF}[s]
    if cl == 12: return DX if s == DX else N0
    return D if s == DX or nw else s            # 13
wdfa = [wstep(cl, s) for cl in range(14) for s in range(8)]
# does a word start here? from the state before and after: 3 yes, 2 if this byte is a dot, 0 no
def wstart(pw, wl):
    if wl < ID: return 0
    if pw < ID: return 3
    if wl == ID: return 0 if pw == ID else 3
    if wl == D: return 3 if pw == DX else 0
    if wl == DF: return {ID: 3, D: 0, DF: 2, H: 0, DX: 3}[pw]
    return 0
wst = [wstart(i & 7, i >> 3) if i < 64 else 0 for i in range(256)]

# ---- 3. operators
OPCH = sorted(set(''.join(OPS)))
fcid = {ch: i + 1 for i, ch in enumerate(OPCH)}                       # 1..23
SC = '=+-<>&|^.'                                                      # second characters of two-character operators
gfc = tab256(lambda c: fcid.get(chr(c), 0))
gsc = tab256(lambda c: SC.index(chr(c)) + 1 if chr(c) in SC else 0)
gsc24 = [24 * x for x in gsc]
gopm = [255 * x for x in gop] if False else tab256(lambda c: 255 if chr(c) in OPCH else 0)
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
strt = [strv(i >> 3, i & 7) if i < 64 else 0 for i in range(256)]      # index: 8 * kind + context
# what a token is, from its first byte: 0 operator, 1 identifier, 2 number, 3 string, 4 rune, 5 newline, 6 '.' (a number if a digit follows)
kww = [[0] * 256 for _ in range(3)]
for k, kind in KW.items():
    w = words(k); i = h_save(kwp, w)
    for j in range(3): kww[j][i] = w[j]

# the kind of a token, decided at its last byte
wkt = [0] * 256                      # words: from the word state at the last byte (+8 if that byte is 'i')
for st_, kd in ((ID, IDENT), (D, INT), (DF, FLOAT), (DX, FLOAT), (H, INT)):
    wkt[st_] = kd; wkt[st_ + 8] = kd if st_ == ID else IMAG
e1k = tab256(lambda c: OPS.get(chr(c), STRING if c in (34, 96) else CHAR if c == 39 else 0) if c < 128 else 0)     # one byte: an operator, a closing quote, or a newline (0)
e2k = [0] * 256; e3k = [0] * 256     # two- and three-byte operators, by (second-last, last) byte
for a_ in OPCH:
    for j_, b_ in enumerate(SC):
        ix = (j_ + 1) * 24 + fcid[a_]
        if a_ + b_ in OPS: e2k[ix] = OPS[a_ + b_]
        for o3 in OPS:
            if len(o3) == 3 and o3[1:] == a_ + b_: e3k[ix] = OPS[o3]
semib = [1 if k in SEMI_AFTER else 0 for k in range(256)]

for n, v in (('gk4', gk4), ('gpk', gpk), ('mcls', mcls), ('mdfa', mdfa), ('esc', esc), ('strt', strt), ('gcat', gcat), ('gb1', gb1), ('glo', glo), ('wcls', wcls), ('wdfa', wdfa), ('wst', wst), ('gfc', gfc), ('gsc24', gsc24), ('gopm', gopm), ('e2t', e2t), ('odfa', odfa),
             ('wkt', wkt), ('e1k', e1k), ('e2k', e2k), ('e3k', e3k), ('semib', semib), ('kwk', kwk), ('kwa', kww[0]), ('kwb', kww[1]), ('kwc', kww[2])):
    L.append(data(n, v))
L += f'''0 1 - >Neg
<text
&text >N
N 1 - >Tm
# One line over the bytes. Left behind: where tokens start (ev) and end (te), and the kind of each (kb),
# decided at its last byte. A newline outside strings counts as a token of kind 0; most are dropped later.
#   k     4 * kind of byte (tables hand out scaled values: a byte multiply is eight instructions);
#         es  escape state (2: this byte is escaped);  b1, nx  the bytes before and after
#   m     context after the byte: 0 code, 1 code (just after */), 2 // comment, 3 /* (just opened), 4 /* comment,
#         5 string, 6 raw string, 7 rune
#   sv    string value of (k, m): see strt
#   code  255 if the byte is program text: in code, not a closing quote, not the '/' that opens a comment
#   wl    word state after the byte (N0 .. DX above);  wf  1 if that is a state inside a word
#   t,pt  what (previous byte, this byte) and (the two before) are as operators: 0 nothing, 1 an operator,
#         2 "..", 3 an operator that '=' extends;  pti  the table index of the pair before
#   os    operator state: how many bytes of the current operator have been read
#   tc    token byte;  ts  first byte of a token;  tem  first byte after a token
#   pp    wl + 8 tc + 16 os of the byte before (one carry for all three);  kp  the kind of the token that just ended
# The runs of zeros move a value to a stack depth whose carry register is free: a line has eight.
bytes :c @gk4 :k back :b1 @gpk + 0 k 28 = /esc :es 2 & + + @mcls \\mdfa :m k + k + @strt :sv m 0 = sv 0 = & c 47 = peek :nx 47 = nx 42 = | & 1 ^ & 1 ^ 1 - :code c @gcat b1 @gb1 + back2 @glo + nx 48 - 10 min 10 = 1 - 128 & + @wcls & \\wdfa :wl drop b1 @gfc c @gsc24 + @e2t :t dup 1 = swap 3 = | t 2 = nx 46 = & | back2 @gfc b1 @gsc24 + :pti @e2t :pt 3 = c 61 = & pt 2 = c 46 = & | dup + + 1 + c @gopm & code & wl 3 min 3 = :wf 1 - & :oc 0 0 oc /odfa :os drop drop drop drop k 4 = m 6 != & nx 0 != & :nl wf | os 0 != | sv 0 != | :tc 1 ^ 1 - 8 & wl + os 16 * + 0 over prev :pp drop drop drop wl dup + dup + dup + pp 7 & :pw + @wst c 46 = 1 + & 0 != os 1 = | nl | sv 5 = | sv 3 = es 2 != & | :ts drop 0 0 0 0 ts %ev drop drop drop drop tc 1 ^ ts | pp 8 & 0 != & :tem drop 0 0 0 0 0 tem %te drop drop drop drop drop pp 48 & :po drop pw 3 min 3 = 0 b1 105 = - 8 & pw + @wkt po 32 = pti @e2k po 48 = pti @e3k b1 @e1k sel sel sel :kp drop 0 0 0 0 0 kp tem ^kb drop drop drop drop drop end
# A token that runs to the end of the input has no end position, so the input must end outside a token.
# Its last byte, if a newline, is left out above (peek is 0 there) and handled with the end of input below.
&ev &te - 0 != 3 * halt
# One line over the tokens, for keywords: an identifier is one if its first twelve bytes hash to a keyword
# and are equal to it. kf: the final kind of every token.
ev :s te over - :len drop dup {{text Neg 4 len 4 min - 8 * >> & :a s 4 + Tm min {{text Neg 4 len 4 - 0 max 4 min - 8 * >> & :b s 8 + Tm min {{text Neg 4 len 8 - 0 max 4 min - 8 * >> & :d drop drop drop a {kwp[0]} * b {kwp[1]} * + d {kwp[2]} * + {kwp[3]} >> 255 & :hk @kwa a = hk @kwb b = & hk @kwc d = & len 12 <= & hk @kwk * :kk drop 'kb :kb0 {IDENT} = kk 0 != & kk kb0 sel swap drop .kf
# a newline is a semicolon if the token before it asks for one; so is the end of the input
'kf :k1 0 != k1 prev @semib | >keepb
'kf @semib dup prev - $Lq
Lq 255 & >Ls
ev 'keepb outif4
N 0 > >Has
~Has 0 * Tm + [text 10 = $Lb
~1 0 * N + Lb - Ls outif4
'kf :k2 0 = {SEMI} * k2 + keepb outif
~1 0 * {SEMI} + Ls outif
'''.split('\n')
sys.stdout.write('\n'.join(L))
