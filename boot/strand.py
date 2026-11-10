#!/usr/bin/env python3
"""Strand scalar reference: stage-0 compiler and interpreter.

Deliberately naive: for-loops, dicts, lists. This file is the specification that the
SIMD compiler (src/compiler.strand) must reproduce byte for byte.

  strand.py compile  prog.strand > a.out     (exit status = compile error number)
  strand.py run      prog.strand < input     (interpret)
  strand.py map      prog.strand             (code address -> source line, for 'runtime check failed')
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec import *

_T = None
def tables():
    global _T
    if _T is None:
        _T = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'stencils.json')))
    return _T

class CompileError(Exception):
    def __init__(self, code): self.code = code

class Tok:
    pass

def analyze(src: bytes):
    """Lex and classify. Returns (tokens, nslots, pool). Raises CompileError(lowest failing check)."""
    text = PRELUDE.encode() + b'\n' + src + b'\n'
    errs = set()
    toks = []
    pool = []
    pos = 0
    for lineno, line in enumerate(text.split(b'\n')[:-1]):
        base = pos
        pos += len(line) + 1
        h = line.find(b'#')
        if h >= 0: line = line[:h]
        if line.count(b'"') % 2: errs.add(E_QUOTE)
        # token chars: non-blank, or anything inside a string
        instr = False
        tc = []
        for i, c in enumerate(line):
            if c == 34:
                opening = not instr
                instr = not instr
                tc.append(True)
                # an opening quote must start a token, a closing quote must end one
                if opening and i > 0 and tc[i - 1]: errs.add(E_QUOTE)
                if not opening and i + 1 < len(line) and line[i + 1] > 32: errs.add(E_QUOTE)
            else:
                tc.append(instr or c > 32)
        i = 0
        first = True
        while i < len(line):
            if not tc[i]:
                i += 1; continue
            j = i
            while j < len(line) and tc[j]: j += 1
            t = Tok()
            t.line, t.first, t.last = lineno, first, False
            first = False
            t.start, t.end, t.raw = base + i, base + j, line[i:j]
            toks.append(t)
            i = j
        if toks and toks[-1].line == lineno: toks[-1].last = True

    for n, t in enumerate(toks):
        t.idx = n
        c0 = t.raw[0]
        c1 = t.raw[1] if len(t.raw) > 1 else 32
        o0, o1, s0 = opclass(min(c0, 127)), opclass(min(c1, 127)), sigil(min(c0, 127))
        sig = s0 > 0 and len(t.raw) > 1 and o1 in (O_LOWER, O_UPPER, O_DIGIT)
        t.sgi = s0 if sig else 0
        t.opc = o1 if sig else o0
        t.name = t.raw[1:] if sig else t.raw
        t.lit = t.opc == O_DIGIT
        t.str = t.opc == O_QUOTE
        t.isname = not t.lit and not t.str
        t.poolidx = len(pool)
        if t.lit:
            if len(t.name) > MAXDIGITS or not t.name.isdigit() or any(b > 57 or b < 48 for b in t.name):
                errs.add(E_LEX)
                pool.append(0)
            else:
                pool.append(int(t.name))
        if t.str:
            pool.extend(t.raw[1:-1])
        if t.isname and len(t.name) > MAXNAME: errs.add(E_NAMELEN)
    if errs: raise CompileError(min(errs))

    # names -> slots, numbered in order of each name's last occurrence
    lastocc = {}
    for t in toks:
        if t.isname: lastocc[t.name] = t.idx
    kwnames = {toks[i].name: i for i in range(K)}
    slot_of = {}
    for name, _ in sorted(lastocc.items(), key=lambda kv: kv[1]):
        if name not in kwnames: slot_of[name] = len(slot_of) + 1
    nslots = len(slot_of)

    for t in toks:
        cls = CLSTAB[t.sgi * 5 + t.opc]
        iskw = t.isname and t.name in kwnames
        if iskw: cls = kwnames[t.name] if t.sgi == 0 else ERR
        if t.first: cls = LEADOF[cls]
        if t.idx < K: cls = NONE
        t.cls = cls
        t.slot = slot_of.get(t.name, 0) if t.isname else 0
    if any(t.cls == ERR for t in toks): raise CompileError(E_TOKEN)

    lines = {}
    for t in toks: lines.setdefault(t.line, []).append(t)
    bad = False
    for lt in lines.values():
        isdata = lt[-1].cls == DATA
        for t in lt:
            t.dataline = isdata
            if isdata and not t.last:
                if (t.sgi == 0 and t.lit) or t.cls == STR: t.cls = NONE
                else: bad = True
            elif t.cls == STR or (t.cls == DATA and not t.last): bad = True
    if bad: raise CompileError(E_DATA)

    depth = 0
    bad = False
    for t in toks:
        t.d0 = depth
        depth += DELTA[t.cls]
        t.code = t.cls < NCODE
        if t.code and (t.d0 < 0 or t.d0 > 8 or (depth == 0) != t.last): bad = True
    if bad: raise CompileError(E_DEPTH)

    T = tables()
    for t in toks:
        t.tid = t.cls * ND + t.d0 if t.code else 0
    if any(t.code and T['slen'][t.tid] == 0 for t in toks): raise CompileError(E_STENCIL)

    for lt in lines.values():
        seen = set()
        for t in lt:
            if t.code and CARRY[t.cls]:
                key = (CARRY[t.cls], t.d0)
                if key in seen: raise CompileError(E_CARRY)
                seen.add(key)
    for lt in lines.values():
        if lt[-1].cls == SCATTER:
            for t in lt:
                if READSVEC[t.cls] and t.slot == lt[-1].slot: raise CompileError(E_ALIAS)
    defined = {t.slot for t in toks if DEFINES[t.cls]}
    for t in toks:
        if USES[t.cls] and t.isname and t.slot not in defined: raise CompileError(E_UNDEF)

    # tee rules: a tee-defined name has exactly one definition; on the tee's own line it may only be
    # re-read (pushed, or its length taken) after a ':' tee, never around a '%' tee
    teedef = {t.slot: t for t in toks if t.cls in (TEE, TEEC)}
    ndefs = {}
    for t in toks:
        if DEFINES[t.cls]: ndefs[t.slot] = ndefs.get(t.slot, 0) + 1
    if any(ndefs[s] != 1 for s in teedef): raise CompileError(E_TEE)
    for t in toks:
        d = teedef.get(t.slot) if t.isname else None
        if d is not None and t is not d and t.line == d.line:
            if not (d.cls == TEE and t.cls in (VEC, LEN) and t.idx > d.idx): raise CompileError(E_TEE)

    for t in toks:
        t.disp = 16 * (nslots + 1) + 4 * t.poolidx if t.lit else 16 * t.slot
    # liveness without analysis: a vector's storage is dead after the line holding its last occurrence
    lastocc_tok = {}
    for t in toks:
        if t.isname: lastocc_tok[t.name] = t.idx
    for t in toks:
        t.dying = (t.isname and t.slot > 0 and t.opc == O_LOWER and t.idx >= K and lastocc_tok[t.name] == t.idx)
    return toks, lines, nslots, pool


def compile_(src: bytes, want_map=False):
    toks, lines, nslots, pool = analyze(src)
    T = tables()
    datasize = 16 * (nslots + 1) + 4 * len(pool)
    vdata = VBASE + 112 + len(T['stub'])
    slotimg = [0] * (4 * (nslots + 1))
    slotimg[0] = datasize
    for lt in lines.values():
        s = lt[-1]
        if s.cls == DATA:
            slotimg[4 * s.slot] = vdata + 16 * (nslots + 1) + 4 * lt[0].poolidx
            slotimg[4 * s.slot + 2] = s.poolidx - lt[0].poolidx
    code = bytearray()
    cmap = []
    def emit(start, n, pos, value):
        blob = bytearray(T['sten'][start:start + n])
        if pos: blob[pos - 1:pos + 3] = struct.pack('<I', value)
        return blob
    for ln in sorted(lines):
        # a line is laid out as: every token's prologue, every token's body, then releases
        for t in lines[ln]:
            if t.code: code += emit(T['qstart'][t.tid], T['qlen'][t.tid], T['qpos'][t.tid], t.disp + T['qadd'][t.tid])
        for t in lines[ln]:
            if not t.code: continue
            cmap.append((len(code), t.line, t.raw.decode('latin1')))
            code += emit(T['sstart'][t.tid], T['slen'][t.tid], T['ppos'][t.tid], t.disp + T['padd'][t.tid])
        for t in lines[ln]:
            if t.dying: code += emit(T['rels'], T['rell'], T['relp'], 16 * t.slot)
    code += bytes(T['tail'])
    fsize = 112 + len(T['stub']) + datasize + len(code)
    out = bytes(T['ehdr']) + struct.pack('<QQ', fsize, fsize) + bytes(T['stub'])
    out += struct.pack('<%dI' % len(slotimg), *slotimg) + struct.pack('<%dI' % len(pool), *pool) + bytes(code)
    assert len(out) == fsize
    if want_map: return out, [(vdata + datasize + off, line, raw) for off, line, raw in cmap]
    return out


# ------------------------------------------------------------------ interpreter
class Fail(Exception): pass          # runtime check failed: exit 99, no output
class TooBig(Exception): pass        # line longer than the interpreter is willing to model (test harness skips)
MAXN = 1 << 20
class Halt(Exception):
    def __init__(self, status): self.status = status

def w32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x & 0x80000000 else x

INT_MIN = -(1 << 31)
BINOPS = {
    '+': lambda a, b: w32(a + b), '-': lambda a, b: w32(a - b), '*': lambda a, b: w32(a * b),
    '&': lambda a, b: a & b, '|': lambda a, b: a | b, '^': lambda a, b: a ^ b,
    'min': min, 'max': max,
    '<<': lambda a, b: w32(a << b) if 0 <= b < 32 else 0,
    '>>': lambda a, b: w32((a & 0xffffffff) >> b) if 0 <= b < 32 else 0,
    '<': lambda a, b: int(a < b), '<=': lambda a, b: int(a <= b), '>': lambda a, b: int(a > b),
    '>=': lambda a, b: int(a >= b), '=': lambda a, b: int(a == b), '!=': lambda a, b: int(a != b),
}

def interp(src: bytes, inp: bytes):
    """Returns (status, output). Compile errors propagate as CompileError."""
    toks, lines, nslots, pool = analyze(src)
    vec = {}      # slot -> list
    sca = {}      # slot -> int
    for lt in lines.values():
        s = lt[-1]
        if s.cls == DATA: vec[s.slot] = pool[lt[0].poolidx:s.poolidx]
    out = bytearray()
    try:
        for ln in sorted(lines):
            lt = lines[ln]
            if not lt[0].code: continue
            st = []
            n = None
            for t in lt:
                c = t.cls
                sval = (pool[t.poolidx] if t.lit else sca.get(t.slot, 0))
                v = vec.get(t.slot, [])
                if c == VECLEAD: n = len(v); st.append(list(v))
                elif c == SCALLEAD: n = 1; st.append([sval])
                elif c == LENLEAD: n = 1; st.append([len(v)])
                elif c == IOTA:
                    if sval < 0: raise Fail()
                    if sval > MAXN: raise TooBig()
                    n = sval; st.append(list(range(n)))
                elif c == KW['input']: n = len(inp); st.append(list(inp))
                elif c == VEC:
                    if len(v) != n: raise Fail()
                    st.append(list(v))
                elif c == SCAL: st.append([sval] * n)
                elif c == LEN: st.append([len(v)] * n)
                elif c == GATHER:
                    idx = st.pop()
                    if any(not 0 <= i < len(v) for i in idx): raise Fail()
                    st.append([v[i] for i in idx])
                elif c < K and KEYWORDS[c] in BINOPS:
                    b = st.pop(); a = st.pop(); f = BINOPS[KEYWORDS[c]]
                    st.append([f(x, y) for x, y in zip(a, b)])
                elif c == KW['sel']:
                    b = st.pop(); a = st.pop(); cnd = st.pop()
                    st.append([x if q else y for q, x, y in zip(cnd, a, b)])
                elif c == KW['scan']:
                    a = st.pop(); r = []; acc = 0
                    for x in a: acc = w32(acc + x); r.append(acc)
                    st.append(r)
                elif c == KW['scanmax']:
                    a = st.pop(); r = []; acc = INT_MIN
                    for x in a: acc = max(acc, x); r.append(acc)
                    st.append(r)
                elif c == KW['prev']:
                    a = st.pop(); st.append(([0] + a)[:len(a)])
                elif c == KW['dup']: st.append(list(st[-1]))
                elif c == KW['drop']: st.pop()
                elif c == KW['swap']: st[-1], st[-2] = st[-2], st[-1]
                elif c == KW['over']: st.append(list(st[-2]))
                elif c == STOREV: vec[t.slot] = st.pop()
                elif c == STORES:
                    a = st.pop()
                    if n != 1: raise Fail()
                    sca[t.slot] = a[0]
                elif c == SUM:
                    acc = 0
                    for x in st.pop(): acc = w32(acc + x)
                    sca[t.slot] = acc
                elif c == MAX: sca[t.slot] = max(st.pop(), default=INT_MIN)
                elif c == COMPRESS:
                    m = st.pop(); a = st.pop()
                    vec[t.slot] = [x for x, q in zip(a, m) if q]
                elif c == SCATTER:
                    idx = st.pop(); a = st.pop()
                    tgt = vec.setdefault(t.slot, [])
                    for x, i in zip(a, idx):
                        if not 0 <= i < len(tgt): raise Fail()
                        tgt[i] = x
                elif c == TEE: vec[t.slot] = list(st[-1])
                elif c == TEEC:
                    m = st.pop(); a = st.pop()
                    vec[t.slot] = [x for x, q in zip(a, m) if q]
                elif c == KW['end']: st.pop()
                elif c == KW['out']: out += bytes(x & 255 for x in st.pop())
                elif c == KW['out4']:
                    for x in st.pop(): out += struct.pack('<I', x & 0xffffffff)
                elif c == KW['halt']:
                    a = st.pop()
                    if n != 1: raise Fail()
                    if a[0] != 0: raise Halt(a[0] & 255)
                else: raise AssertionError(c)
            assert not st
    except Fail: return 99, b''
    except Halt as h: return h.status, b''
    return 0, bytes(out)


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    src = open(path, 'rb').read()
    try:
        if cmd == 'compile':
            sys.stdout.buffer.write(compile_(src))
        elif cmd == 'run':
            status, out = interp(src, sys.stdin.buffer.read())
            sys.stdout.buffer.write(out); sys.exit(status)
        elif cmd == 'map':
            for addr, line, raw in compile_(src, True)[1]:
                print('%x line %d  %s' % (addr, line, raw))   # line 0 is the prelude, so this is 1-based in the file
    except CompileError as e:
        sys.exit(e.code)

if __name__ == '__main__':
    main()
