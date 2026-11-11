#!/usr/bin/env python3
"""godecl_gen.py > godecl.strand -- the package-level declarations of Go source, in Strand.

Input: Go source (any number of files, one after the other). Output: for every name declared at package
level -- functions, methods, variables, constants, types -- the byte offset of the name (little-endian
dwords), then one byte each: 1 func, 2 method, 3 var, 4 const, 5 type, 6 the path of an import, 7 the name in a
package clause, 8 and 9 the braces around the body of a function or method. `golex decls` is the reference:
go/parser, and a walk over each file's declarations.

It is the lexer of golex_gen.py and three more lines, which do what a parser does first:
  1. nesting: +1 for every ( [ {, -1 for every ) ] }, and a running sum over the tokens;
  2. an 8-state automaton over the tokens, reading each token's kind and how deeply it is nested, follows
     declarations: after `func`, a name (after a receiver, if there is one); after var / const / type, a list
     of names, or a parenthesised group with one such list per statement; everything else is skipped to the
     next semicolon at the right depth. It is in state VL or GL exactly at a declared name;
  3. a second automaton remembers the last of func / var / const / type / import seen outside all brackets:
     a string while that is `import` is an import path. The name after `package` is listed too, so the
     output says where each file begins and what it imports -- the edges of the build graph.
Semicolons are the lexer's: the automatic ones are what make "one statement" visible without parsing."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golex_gen as G
L = [l for l in G.L if 'outif' not in l]                 # the lexer, without its output
IDENT, COMMA, SEMI, LP, RP = 4, 52, 57, 49, 54
OPEN, CLOSE = (49, 50, 51), (54, 55, 56)
FUNC, VAR, CONST, TYPE, IMPORT, PACKAGE = 71, 85, 64, 84, 75, 78
def data(name, vals): return ' '.join(str(v) for v in vals) + ' =' + name
tab = lambda f: [f(k) for k in range(256)]
dlt = tab(lambda k: 2 if k in OPEN else 0 if k in CLOSE else 1)          # 1 + what the token adds to the depth
# kind classes: 0 no token (a newline that is not a semicolon) 1 func 2 var/const/type 3 import/package 4 identifier
# 5 comma 6 semicolon 7 ( 8 ) 9 anything else
kcl = tab(lambda k: 0 if k == 0 else 1 if k == FUNC else 2 if k in (VAR, CONST, TYPE) else 3 if k in (IMPORT, PACKAGE) else
          4 if k == IDENT else 5 if k == COMMA else 6 if k == SEMI else 7 if k == LP else 8 if k == RP else 9)
# the automaton's input, from kind class and depth (of a bracket: the depth it opens or closes at; 2 = two or more):
#   0 no token   1 anything nested twice or more
#   outside all brackets:  2 func 3 var/const/type 4 import/package 5 identifier 6 comma 7 semicolon 8 ( 9 ) 10 other
#   inside one:            11 identifier 12 comma 13 semicolon 14 other
def dclass(kc, pd):
    if kc == 0: return 0
    if pd >= 2: return 1
    if pd == 0: return kc + 1
    return {4: 11, 5: 12, 6: 13}.get(kc, 14)
dcls = [dclass(i & 15, i >> 4) if i & 15 < 10 and i >> 4 < 3 else 0 for i in range(256)]
# states: T between declarations; F after `func`; V after var/const/type, or after a comma in its list of names;
# VL at a declared name; SK skipping to the end of the declaration; G at the start of a statement in a
# parenthesised group; GL at a name declared there; GSK skipping to the end of that statement
T, F, V, VL, SK, GG, GL, GSK = range(8)
def dstep(c, s):
    if c in (0, 1): return s
    grp = s in (GG, GL, GSK)
    if c == 2: return F if s == T else s if grp else SK
    if c == 3: return V if s == T else s if grp else SK
    if c == 4: return s if grp else SK
    if c == 5: return VL if s in (F, V) else s if grp or s == T else SK
    if c == 6: return V if s == VL else s if grp or s in (T, F) else SK
    if c == 7: return T
    if c == 8: return GG if s == V else s if grp or s in (T, F) else SK
    if c == 9: return SK if grp or s in (V, VL) else s
    if c == 10: return s if grp or s == T else SK
    if s in (T, F, SK): return s
    if not grp: return SK
    if c == 11: return GL if s == GG else GSK
    if c == 12: return GG if s == GL else GSK
    if c == 13: return GG
    return GSK
ddfa = [dstep(c, s) for c in range(15) for s in range(8)]
k2c = tab(lambda k: {FUNC: 1, VAR: 2, CONST: 3, TYPE: 4, IMPORT: 5, PACKAGE: 6, 51: 7, 56: 8}.get(k, 0))
# ... but `func` counts only where it begins a declaration (the first automaton is then in F), var / const / type
# likewise (V), and a { only if it does not follow `struct` or `interface`: index = class + 16 * state + 128 * that
isst = tab(lambda k: 128 if k in (82, 76) else 0)
t2 = [0 if (c == 1 and d != F) or (c in (2, 3, 4) and d != V) or (c == 7 and st) else c
      for i in range(256) for c, d, st in [(i & 15, (i >> 4) & 7, i >> 7)]]
# states: 0 func (its body not yet open) 1 var 2 const 3 type 4 import 5 package 6 in the body of a function 7 after it
def kstep(c, s):
    if 1 <= c <= 6: return c - 1
    if c == 7: return 6 if s == 0 else s
    if c == 8: return 7 if s == 6 else s
    return s
kdfa = [kstep(c, s) for c in range(9) for s in range(8)]
kbr = tab(lambda k: 32 if k == 51 else 64 if k == 56 else 0)
# what a listed token is, by keyword state, + 8 if the token before is ')', + 16 if it is `package`
# ... + 32 for a {, + 64 for a }
symk = [8 if i >> 5 == 1 else 9 if i >> 5 == 2 else 7 if i & 16 else 2 if i & 15 == 8 else [1, 3, 4, 5, 6, 0, 0, 0][i & 7] for i in range(128)] + [0] * 128
STRING = 9
for n, v in (('dlt', dlt), ('kcl', kcl), ('dcls', dcls), ('ddfa', ddfa), ('k2c', k2c), ('isst', isst), ('t2', t2), ('kdfa', kdfa), ('kbr', kbr), ('symk', symk)): L.append(data(n, v))
L += f'''# tkind: every token's kind, newlines that count as semicolons included (0: a newline that does not); dl: 1 + its effect on the depth
'kf :dk 0 = {SEMI} * dk + 0 keepb - & :tkind @dlt >dl
# pd: how deeply each token is nested (0, 1, or 2 for more); a bracket is as deep as what it stands in
ev 0 * 'dl + 1 - :ddt scan ddt 0 max - 2 min .pd
# dst: the declaration automaton (above); a name is declared where it is VL or GL
'tkind :dq @kcl pd dup + dup + dup + dup + + @dcls \\ddfa :dst dq @k2c dst dup + dup + dup + dup + + back :dpb @isst + @t2 pd 0 = 0 swap - & \\kdfa :dks dpb {RP} = dup + dup + dup + + dpb {PACKAGE} = dup + dup + dup + dup + + dq @kbr :dbr + @symk :dsy drop {VL} = dst {GL} = | dq {STRING} = dks 4 = & | dq {IDENT} = dpb {PACKAGE} = & | dbr 32 = dks 6 = & dbr 64 = dks 7 = & | pd 0 = & | :dem 0 0 dsy dem ^sk dem %si drop drop end
si @ev out4
'sk out
'''.split('\n')
if __name__ == '__main__': sys.stdout.write('\n'.join(L))
