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
 STOREV, STORES, SUM, MAX, COMPRESS, SCATTER, TEE, TEEC, TEEL, VECL, LENL) = range(K, K + 19)
NCODE = K + 19                      # classes below this emit code
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
for c in [VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, IOTA, VECL, LENL]:
    DELTA[c] = 1
for c in [STOREV, STORES, SUM, MAX]:
    DELTA[c] = -1
for c in [COMPRESS, SCATTER, TEEC]:
    DELTA[c] = -2

LEADOF = list(range(NCLS))          # class a token takes when it is first on its line
LEADOF[VEC], LEADOF[SCAL], LEADOF[LEN] = VECLEAD, SCALLEAD, LENLEAD

# Chunk-local vectors. A ':x' tee whose name never occurs after its own line does not need an array:
# its occurrences on that line use these classes, which keep just the current 64-byte chunk in x's slot.
LOCALOF = list(range(NCLS))
LOCALOF[TEE], LOCALOF[VEC], LOCALOF[LEN] = TEEL, VECL, LENL
SLOT = 64                           # bytes per slot: {ptr, len} or one zmm chunk

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
for c in [STOREV, STORES, SUM, MAX, COMPRESS, DATA, TEE, TEEC, TEEL]: DEFINES[c] = 1
USES = [0] * NCLS
for c in [VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, GATHER, IOTA, SCATTER, VECL, LENL]: USES[c] = 1
READSVEC = [0] * NCLS               # reads vector *contents* (conflicts with an in-place scatter)
for c in [VEC, VECLEAD, GATHER, VECL]: READSVEC[c] = 1
CARRY = [0] * NCLS                  # 1: uses sum-carry register of its depth, 2: max-carry
CARRY[KW['scan']] = 1; CARRY[KW['prev']] = 1; CARRY[KW['scanmax']] = 2

MAXNAME = 64                        # longest name (hash weight table size)
MAXDIGITS = 9

# ---- name resolution is part of the language definition -------------------------------------------
# Names are told apart by a 64-bit fingerprint and resolved by a 4-round "scatter race".
# A program whose names cannot be resolved this way is rejected (E_RACE / E_COLLISION), by *every*
# implementation, so acceptance never depends on which compiler you run.
M32 = 0xffffffff
HA1, HB1, HS1 = 668265263, 374761393, 15
HA2, HB2, HS2 = 951274213, 246822519, 13
RACE_U = [1, 40503, 48271, 69621]
RACE_V = [961748927, 314159265, 271828183, 141421357]
ROUNDS = 4
def name_hash(name: bytes):
    """fingerprint = two sums over the name's bytes of a mixed (byte, offset) value"""
    h1 = h2 = 0
    for k, c in enumerate(name):
        x = c + 256 * k
        a = (x * HA1) & M32; h1 = (h1 + (a ^ (a >> HS1)) * HB1) & M32
        a = (x * HA2) & M32; h2 = (h2 + (a ^ (a >> HS2)) * HB2) & M32
    return h1, h2
def race_mask(ntokens):
    """table size - 1: twice the smallest power of two above the token count"""
    m = ntokens
    for sh in (1, 2, 4, 8, 16): m |= m >> sh
    return 2 * m + 1
def race_key(r, h, mask):
    return (((h[0] * RACE_U[r] + h[1] * RACE_V[r]) & M32) >> 8) & mask
VBASE = 0x400000

# compile errors, in priority order: exit status = lowest failing check
(E_QUOTE, E_LEX, E_NAMELEN, E_RACE, E_COLLISION, E_TOKEN, E_DATA, E_DEPTH, E_STENCIL,
 E_CARRY, E_ALIAS, E_UNDEF, E_TEE) = range(1, 14)
