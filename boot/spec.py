"""Strand: the constants every implementation must agree on.

Class numbers index the stencil table (tid = cls*9 + depth_before).
The first K classes are the keywords, in prelude order.
"""
KEYWORDS = ['+', '-', '*', '&', '|', '^', 'min', 'max', '<<', '>>',
            '<', '<=', '>', '>=', '=', '!=', 'sel', 'scan', 'scanmax', 'prev',
            'dup', 'drop', 'swap', 'over', 'input', 'out', 'out4', 'halt', 'end', 'idx']
K = len(KEYWORDS)
PRELUDE = ' '.join(KEYWORDS)

(VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, GATHER, IOTA,
 STOREV, STORES, SUM, MAX, COMPRESS, SCATTER, TEE, TEEC) = range(K, K + 16)
NCODE = K + 16                      # classes below this emit code
DATA, STR, NONE, ERR = NCODE, NCODE + 1, NCODE + 2, NCODE + 3
NCLS = NCODE + 4
ND = 9                              # depth-before ranges over 0..8

KW = {k: i for i, k in enumerate(KEYWORDS)}
DELTA = [0] * NCLS
for k in ['+', '-', '*', '&', '|', '^', 'min', 'max', '<<', '>>', '<', '<=', '>', '>=', '=', '!=',
          'drop', 'out', 'out4', 'halt', 'end']:
    DELTA[KW[k]] = -1
DELTA[KW['sel']] = -2
for k in ['dup', 'over', 'input', 'idx']:
    DELTA[KW[k]] = 1
for c in [VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, IOTA]:
    DELTA[c] = 1
for c in [STOREV, STORES, SUM, MAX]:
    DELTA[c] = -1
for c in [COMPRESS, SCATTER, TEEC]:
    DELTA[c] = -2

LEADOF = list(range(NCLS))          # class a token takes when it is first on its line
LEADOF[VEC], LEADOF[SCAL], LEADOF[LEN] = VECLEAD, SCALLEAD, LENLEAD

# lexical classes of a byte
O_OTHER, O_LOWER, O_UPPER, O_DIGIT, O_QUOTE = range(5)
def opclass(c):
    if 97 <= c <= 122 or c == 95: return O_LOWER
    if 65 <= c <= 90: return O_UPPER
    if 48 <= c <= 57: return O_DIGIT
    if c == 34: return O_QUOTE
    return O_OTHER
SIGILS = '>@~?!$^&=:%'                # sigil index = position + 1
def sigil(c):
    return SIGILS.index(chr(c)) + 1 if c < 128 and chr(c) in SIGILS else 0

# CLSTAB[sigil*5 + opclass] = class of a non-keyword token
CLSTAB = [ERR] * ((len(SIGILS) + 1) * 5)
def _s(sg, oc, cls): CLSTAB[(SIGILS.index(sg) + 1 if sg else 0) * 5 + oc] = cls
_s('', O_LOWER, VEC); _s('', O_UPPER, SCAL); _s('', O_DIGIT, SCAL); _s('', O_QUOTE, STR)
_s('>', O_LOWER, STOREV); _s('>', O_UPPER, STORES)
_s('@', O_LOWER, GATHER)
_s('~', O_UPPER, IOTA); _s('~', O_DIGIT, IOTA)
_s('?', O_LOWER, COMPRESS); _s('!', O_LOWER, SCATTER)
_s('$', O_UPPER, SUM); _s('^', O_UPPER, MAX)
_s('&', O_LOWER, LEN); _s('=', O_LOWER, DATA)
_s(':', O_LOWER, TEE); _s('%', O_LOWER, TEEC)

# which classes define / use their operand variable
DEFINES = [0] * NCLS
for c in [STOREV, STORES, SUM, MAX, COMPRESS, DATA, TEE, TEEC]: DEFINES[c] = 1
USES = [0] * NCLS
for c in [VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, GATHER, IOTA, SCATTER]: USES[c] = 1
READSVEC = [0] * NCLS               # reads vector *contents* (conflicts with an in-place scatter)
for c in [VEC, VECLEAD, GATHER]: READSVEC[c] = 1
CARRY = [0] * NCLS                  # 1: uses sum-carry register of its depth, 2: max-carry
CARRY[KW['scan']] = 1; CARRY[KW['prev']] = 1; CARRY[KW['scanmax']] = 2

MAXNAME = 64                        # longest name (hash weight table size)
MAXDIGITS = 9
TABM = 65536                        # scatter-race table size
RACE_MULT = [1, 40503, 48271]       # round r uses ((h * mult) >> 16) & (TABM-1); round 0 is h's own top bits
VBASE = 0x400000

# compile errors, in priority order: exit status = lowest failing check
(E_QUOTE, E_LEX, E_NAMELEN, E_RACE, E_COLLISION, E_TOKEN, E_DATA, E_DEPTH, E_STENCIL,
 E_CARRY, E_ALIAS, E_UNDEF, E_TEE) = range(1, 14)
