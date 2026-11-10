"""Strand: the constants every implementation must agree on.
The program text is the source followed by one newline; tokens are numbered from 0.

Class numbers index the stencil table (tid = cls*9 + depth_before).
The first K classes are the keywords, in prelude order.
"""
KEYWORDS = ['+', '-', '*', '&', '|', '^', 'min', 'max', '<<', '>>',
            '<', '<=', '>', '>=', '=', '!=', 'sel', 'scan', 'scanmax', 'prev',
            'dup', 'drop', 'swap', 'over', 'input', 'out', 'out4', 'halt', 'end', 'idx', 'input4', 'outif', 'outif4', 'bytes']
K = len(KEYWORDS)
PRELUDE = ' '.join(KEYWORDS)

(VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, GATHER, IOTA,
 STOREV, STORES, SUM, MAX, COMPRESS, SCATTER, TEE, TEEC, TEEL, VECL, LENL,
 SCALB, TABLEB, SUMB, TEEBX, VECBX, BAD, VECT, FSM, FSMB,
 STOREVB, WHEREB, GATHERB, GATHER4,
 STOREVN, BYTESV, SLSRC, SLICES, SCATTER4) = range(K, K + 37)
BK = K + 37                         # byte-line variant of keyword k is class BK + k
NCODE = BK + K                      # classes below this emit code
DATA, STR, NONE, ERR = NCODE, NCODE + 1, NCODE + 2, NCODE + 3
NCLS = NCODE + 4
ND = 9                              # depth-before ranges over 0..8

KW = {k: i for i, k in enumerate(KEYWORDS)}
DELTA = [0] * NCLS
for k in ['+', '-', '*', '&', '|', '^', 'min', 'max', '<<', '>>', '<', '<=', '>', '>=', '=', '!=',
          'drop', 'out', 'out4', 'halt', 'end']:
    DELTA[KW[k]] = -1
DELTA[KW['sel']] = -2
DELTA[KW['outif']] = DELTA[KW['outif4']] = -2
for k in ['dup', 'over', 'input', 'idx', 'input4', 'bytes']:
    DELTA[KW[k]] = 1
for c in [VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, IOTA, VECL, LENL]:
    DELTA[c] = 1
for c in [STOREV, STORES, SUM, MAX]:
    DELTA[c] = -1
for c in [COMPRESS, SCATTER, TEEC]:
    DELTA[c] = -2

for _k in range(K): DELTA[BK + _k] = DELTA[_k]
DELTA[SCALB] = 1; DELTA[VECBX] = 1; DELTA[SUMB] = -1; DELTA[VECT] = 1
DELTA[STOREVB] = -1; DELTA[WHEREB] = -1
DELTA[STOREVN] = -1; DELTA[BYTESV] = 1; DELTA[SLICES] = -3; DELTA[SCATTER4] = -2

LEADOF = list(range(NCLS))          # class a token takes when it is first on its line
LEADOF[VEC], LEADOF[SCAL], LEADOF[LEN] = VECLEAD, SCALLEAD, LENLEAD

# Chunk-local vectors. A ':x' tee whose name never occurs after its own line does not need an array:
# its occurrences on that line use these classes, which keep just the current 64-byte chunk in x's slot.
LOCALOF = list(range(NCLS))
LOCALOF[TEE], LOCALOF[VEC], LOCALOF[LEN] = TEEL, VECL, LENL
SLOT = 64                           # bytes per slot: {ptr, len, byte-table ptr} or one zmm chunk

# Byte lines. A line whose lead is `bytes` runs with 64 unsigned byte lanes per chunk instead of 16
# dword lanes. Every token on it takes the class BYTEOF[cls]; classes with no byte meaning map to BAD,
# which has no stencil. Named vectors do not exist in byte lines except as chunk-local tees.
BYTEOF = list(range(NCLS))
for _c in range(NCODE): BYTEOF[_c] = BAD
for _k in range(K): BYTEOF[_k] = BK + _k
BYTEOF[SCAL], BYTEOF[GATHER], BYTEOF[SUM], BYTEOF[TEE], BYTEOF[VEC] = SCALB, TABLEB, SUMB, TEEBX, VECBX
# Automata. `x /t` on a byte line replaces each element by the state of a 4-state automaton after
# reading it: state' = t[(x & 15) * 4 + state] & 3 (entries past the end of t, or past 64, are 0);
# the state before the first element is 0. It exists on byte lines only (FSM itself has no stencil).
BYTEOF[FSM] = FSMB
# The bridge between byte lines and dword lines. A byte line can leave two kinds of vector behind:
#   `>x`   a byte vector: its elements are the line's bytes (`:x` the same, and the line goes on);
#   `m %x` a dword vector of *positions*: the indices of the lanes where m is nonzero, in order.
# A dword line reads a byte vector by index: `i [x` is the byte at i, `i {x` the little-endian dword
# that starts at byte i (bytes past the end of x read as 0). An index outside 0..len-1 is a runtime
# failure. A name is a byte vector or a dword vector for the whole program (E_TYPE).
BYTEOF[STOREV] = STOREVB; BYTEOF[TEEC] = WHEREB
# Building byte vectors from dword lines -- what a code generator and a linker do.
#   `v .x`               (sink) the low byte of every lane, stored as the byte vector x
#   `'x`                 (lead) a byte line over the byte vector x, as `bytes` is over stdin
#   `s n d (src )dst`    copy n bytes from src[s..] to dst[d..], one slice per lane, in lane order.
#                        `(src` names the source for its line (it leaves the stack alone); `)dst` pops
#                        the three operands and the line goes on. A slice outside either vector, a
#                        negative operand, or src and dst being one vector is a runtime failure.
#   `v p }x`             (sink) store the dword v at byte offset p of x; a negative p is a masked-off
#                        lane, p + 4 > len a runtime failure. Overlapping stores land in lane order.
BYTEOF[BYTESV] = BYTESV
ISBLEAD = [0] * NCLS                # leads that make their line a byte line
ISBLEAD[KW['bytes']] = ISBLEAD[BYTESV] = 1
ISSCAT = [0] * NCLS                 # sinks that write into an existing vector (a line may not also read it)
ISSCAT[SCATTER] = ISSCAT[SCATTER4] = 1
ISTEEC = [0] * NCLS                 # '%' compress-tees: one definition, never re-read on their own line
ISTEEC[TEEC] = ISTEEC[WHEREB] = 1
VTYPE = [0] * NCLS                  # what a class requires of its operand name: 1 dword vector, 2 byte vector
for _c in (VEC, VECLEAD, VECT, GATHER, STOREV, COMPRESS, SCATTER, TEE, TEEC, WHEREB, DATA, TABLEB, FSM, FSMB): VTYPE[_c] = 1
for _c in (STOREVB, TEEBX, GATHERB, GATHER4, STOREVN, BYTESV, SLSRC, SLICES, SCATTER4): VTYPE[_c] = 2
LOCALOF[TEEBX], LOCALOF[VECBX] = TEEL, VECL
ISTEE = [0] * NCLS                  # ':' tees, before chunk-local classification
ISTEE[TEE] = ISTEE[TEEBX] = 1
REREAD = [0] * NCLS                 # what may re-read a ':' tee later on its own line
REREAD[VEC] = REREAD[LEN] = REREAD[VECBX] = 1
# A ':x' that outlives its line and is pushed again on that line: the push (class VECT) reads the array
# while it is being written, so a pool warm-up chunk (which stores nothing) would read stale data.
# VECT is VEC plus a veto: such a line always runs on one thread. (Found by Codex, review 3.)

# lexical classes of a byte
O_OTHER, O_LOWER, O_UPPER, O_DIGIT, O_QUOTE = range(5)
def opclass(c):
    if 97 <= c <= 122 or c == 95: return O_LOWER
    if 65 <= c <= 90: return O_UPPER
    if 48 <= c <= 57: return O_DIGIT
    if c == 34: return O_QUOTE
    return O_OTHER
SIGILS = '>@~?!$^&=:%/[{.\'()}'                # sigil index = position + 1
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
_s(':', O_LOWER, TEE); _s('%', O_LOWER, TEEC); _s('/', O_LOWER, FSM)
_s('[', O_LOWER, GATHERB); _s('{', O_LOWER, GATHER4)
_s('.', O_LOWER, STOREVN); _s("'", O_LOWER, BYTESV); _s('(', O_LOWER, SLSRC); _s(')', O_LOWER, SLICES); _s('}', O_LOWER, SCATTER4)

# which classes define / use their operand variable
DEFINES = [0] * NCLS
for c in [STOREV, STORES, SUM, MAX, COMPRESS, DATA, TEE, TEEC, TEEL, SUMB, TEEBX, STOREVB, WHEREB, STOREVN]: DEFINES[c] = 1
USES = [0] * NCLS
for c in [VEC, VECLEAD, SCAL, SCALLEAD, LEN, LENLEAD, GATHER, IOTA, SCATTER, VECL, LENL, SCALB, TABLEB, VECBX, FSM, FSMB, GATHERB, GATHER4, BYTESV, SLSRC, SLICES, SCATTER4]: USES[c] = 1
READSVEC = [0] * NCLS               # reads vector *contents* (conflicts with an in-place scatter)
for c in [VEC, VECLEAD, GATHER, VECL, TABLEB, VECBX, FSM, FSMB, GATHERB, GATHER4, BYTESV, SLSRC]: READSVEC[c] = 1
CARRY = [0] * NCLS                  # 1: uses sum-carry register of its depth, 2: max-carry
CARRY[KW['scan']] = 1; CARRY[KW['prev']] = 1; CARRY[KW['scanmax']] = 2
CARRY[BK + KW['prev']] = 1
CARRY[FSMB] = 1                    # the automaton's carry (a transition function) lives in zmm16+r too
USES[VECT] = 1; READSVEC[VECT] = 1

MAXNAME = 16                        # longest name: four dwords, which *are* the name
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
def name_words(name: bytes):
    """a name as four little-endian dwords, zero-padded (a name has no zero bytes and at most 16)"""
    b = name[:MAXNAME].ljust(MAXNAME, b'\0')
    return [int.from_bytes(b[4 * j:4 * j + 4], 'little') for j in range(4)]
def name_hash(name: bytes):
    """fingerprint = two sums over the name's four dwords of a mixed (dword + position) value.
    The whole name is visible from its token as four gathers, so nothing here needs a pass over bytes."""
    h1 = h2 = 0
    for j, w in enumerate(name_words(name)):
        x = (w + j) & M32
        a = (x * HA1) & M32; h1 = (h1 + (a ^ (a >> HS1)) * HB1) & M32
        a = (x * HA2) & M32; h2 = (h2 + (a ^ (a >> HS2)) * HB2) & M32
    return h1, h2
# Keywords are names with a fixed meaning. They take no part in the race and have no slot. How an
# implementation recognises them is its own business; for table-driven ones, KWSHIFT is a shift for
# which (h1 >> KWSHIFT) & 255 is different for every keyword.
KWSHIFT = next(s for s in range(25) if len({(name_hash(k.encode())[0] >> s) & 255 for k in KEYWORDS}) == K)
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
 E_CARRY, E_ALIAS, E_UNDEF, E_TEE, E_TYPE) = range(1, 15)
