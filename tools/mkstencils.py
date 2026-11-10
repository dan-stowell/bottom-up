#!/usr/bin/env python3
"""Generate every stencil (class x stack depth), assemble with GNU as, extract bytes.

Outputs build/stencils.json (for the Python reference) and src/tables.strand
(data lines that become part of the Strand compiler's own source).
"""
import json, os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'boot'))
from spec import *

ROOT = os.path.join(os.path.dirname(__file__), '..')
B = os.path.join(ROOT, 'build')
os.makedirs(B, exist_ok=True)
DISP = '0x11223344'

LOOPEND = """
  add rax,16
  cmp rax,r12
  jae 1f
  jmp r11
1:"""
def failif(cc_ok):   # continue if cc_ok, else call the fail handler
    return f"""
  j{cc_ok} 2f
  call r14
2:"""
HEAD = """
  mov rdi,r13
  mov r8,r13
  xor eax,eax
""" + ''.join(f"  vpxord zmm{r},zmm{r},zmm{r}\n" for r in range(16, 24)) + """
  vpslld zmm29,zmm31,31
""" + ''.join(f"  vmovdqa32 zmm{r},zmm29\n" for r in range(8, 16)) + """
  vpxord zmm28,zmm28,zmm28
  lea r11,[rip+0]
  mov rcx,r12
  sub rcx,rax
  mov esi,16
  cmp rcx,rsi
  cmova rcx,rsi
  mov esi,-1
  bzhi esi,esi,ecx
  kmovw k7,esi
"""
def hred(acc, op):   # horizontal reduce zmm{acc} -> eax
    return f"""
  vextracti64x4 ymm24,zmm{acc},1
  {op} ymm24,ymm24,ymm{acc}
  vextracti32x4 xmm25,ymm24,1
  {op} xmm24,xmm24,xmm25
  vpshufd xmm25,xmm24,0x4e
  {op} xmm24,xmm24,xmm25
  vpshufd xmm25,xmm24,0x55
  {op} xmm24,xmm24,xmm25
  vmovd eax,xmm24"""
def bounds(r):       # k1 = active lanes of zmm{r} that are < len at [rcx+8]; fail if any active lane is not
    return f"""
  vpcmpud k1{{k7}},zmm{r},dword ptr [rcx+8]{{1to16}},1
  kxorw k2,k1,k7
  kortestw k2,k2""" + failif('z')

S = {}   # (cls, d0) -> asm text ; a line containing 'P:' marks "disp32 ends here"
def st(cls, d, text): S[(cls, d)] = text

BIN = {'+': 'vpaddd', '-': 'vpsubd', '*': 'vpmulld', '&': 'vpandd', '|': 'vpord', '^': 'vpxord',
       'min': 'vpminsd', 'max': 'vpmaxsd', '<<': 'vpsllvd', '>>': 'vpsrlvd'}
CMP = {'<': 1, '<=': 2, '>': 6, '>=': 5, '=': 0, '!=': 4}
for d in range(2, 9):
    a, b = d - 2, d - 1
    for k, ins in BIN.items(): st(KW[k], d, f"  {ins} zmm{a},zmm{a},zmm{b}")
    for k, imm in CMP.items(): st(KW[k], d, f"  vpcmpd k1,zmm{a},zmm{b},{imm}\n  vmovdqa32 zmm{a}{{k1}}{{z}},zmm31")
    st(KW['drop'], d, "  nop")
    st(KW['swap'], d, f"  vmovdqa32 zmm24,zmm{a}\n  vmovdqa32 zmm{a},zmm{b}\n  vmovdqa32 zmm{b},zmm24")
    if d <= 7: st(KW['over'], d, f"  vmovdqa32 zmm{d},zmm{a}")
for d in range(3, 9):
    c, a, b = d - 3, d - 2, d - 1
    st(KW['sel'], d, f"  vptestmd k1,zmm{c},zmm{c}\n  vpblendmd zmm{c}{{k1}},zmm{b},zmm{a}")
for d in range(1, 9):
    r = d - 1
    def scan(op, fill, carry):
        t = fill + '\n'
        for sh in (15, 14, 12, 8):
            t += f"  valignd zmm24,zmm{r},zmm25,{sh}\n  {op} zmm{r},zmm{r},zmm24\n"
        return t + f"  {op} zmm{r},zmm{r},zmm{carry}\n  valignd zmm24,zmm{r},zmm{r},15\n  vpbroadcastd zmm{carry},xmm24"
    st(KW['scan'], d, scan('vpaddd', '  vpxord zmm25,zmm25,zmm25', 16 + r))
    st(KW['scanmax'], d, scan('vpmaxsd', '  vpslld zmm25,zmm31,31', 8 + r))
    st(KW['prev'], d, f"  valignd zmm24,zmm{r},zmm{16+r},15\n  vmovdqa32 zmm{16+r},zmm{r}\n  vmovdqa32 zmm{r},zmm24")
    if d <= 7: st(KW['dup'], d, f"  vmovdqa32 zmm{d},zmm{r}")
    st(GATHER, d, f"  lea rcx,[rbx+{DISP}]\nP:" + bounds(r) + f"""
  mov rsi,[rcx]
  vpxord zmm24,zmm24,zmm24
  vpgatherdd zmm24{{k1}},[rsi+zmm{r}*4]
  vmovdqa32 zmm{r},zmm24""")
    if d <= 7:
        st(VEC, d, f"  lea rcx,[rbx+{DISP}]\nP:\n  cmp r12,[rcx+8]" + failif('e') + f"""
  mov rsi,[rcx]
  vmovdqu32 zmm{d}{{k7}}{{z}},[rsi+rax*4]""")
        st(SCAL, d, f"  vpbroadcastd zmm{d},dword ptr [rbx+{DISP}]\nP:")
        st(LEN, d, f"  lea rcx,[rbx+{DISP}]\nP:\n  vpbroadcastd zmm{d},dword ptr [rcx+8]")

st(VECLEAD, 0, f"  lea rdx,[rbx+{DISP}]\nP:\n  mov r12,[rdx+8]\n" + HEAD + "  mov rsi,[rdx]\n  vmovdqu32 zmm0{k7}{z},[rsi+rax*4]")
st(SCALLEAD, 0, "  mov r12d,1\n" + HEAD + f"  vpbroadcastd zmm0,dword ptr [rbx+{DISP}]\nP:")
st(LENLEAD, 0, "  mov r12d,1\n" + HEAD + f"  lea rcx,[rbx+{DISP}]\nP:\n  vpbroadcastd zmm0,dword ptr [rcx+8]")
st(IOTA, 0, f"  mov r12d,dword ptr [rbx+{DISP}]\nP:\n  test r12d,r12d" + failif('ns') + HEAD + "  vpbroadcastd zmm0,eax\n  vpaddd zmm0,zmm0,zmm30")
st(KW['input'], 0, "  mov r12,[rbx-8]\n" + HEAD + "  mov rsi,[rbx-16]\n  vpmovzxbd zmm0{k7}{z},xmmword ptr [rsi+rax]")
st(STOREV, 1, "  vmovdqu32 [rdi+rax*4]{k7},zmm0" + LOOPEND + f"""
  lea rcx,[rbx+{DISP}]
P:
  mov [rcx],rdi
  mov [rcx+8],r12
  lea r13,[rdi+r12*4+127]
  and r13,-64""")
st(STORES, 1, LOOPEND + "\n  cmp r12,1" + failif('e') + f"\n  vmovd dword ptr [rbx+{DISP}],xmm0\nP:")
st(SUM, 1, "  vpaddd zmm28{k7},zmm28,zmm0" + LOOPEND + hred(28, 'vpaddd') + f"\n  mov dword ptr [rbx+{DISP}],eax\nP:")
st(MAX, 1, "  vpmaxsd zmm29{k7},zmm29,zmm0" + LOOPEND + hred(29, 'vpmaxsd') + f"\n  mov dword ptr [rbx+{DISP}],eax\nP:")
st(COMPRESS, 2, """  vptestmd k1{k7},zmm1,zmm1
  vpcompressd zmm24{k1}{z},zmm0
  vmovdqu32 [r8],zmm24
  kmovw ecx,k1
  popcnt ecx,ecx
  lea r8,[r8+rcx*4]""" + LOOPEND + f"""
  lea rcx,[rbx+{DISP}]
P:
  mov [rcx],rdi
  sub r8,rdi
  shr r8,2
  mov [rcx+8],r8
  lea r13,[rdi+r8*4+127]
  and r13,-64""")
st(SCATTER, 2, f"  lea rcx,[rbx+{DISP}]\nP:" + bounds(1) + """
  mov rsi,[rcx]
  vpscatterdd [rsi+zmm1*4]{k1},zmm0""" + LOOPEND)
st(KW['out'], 1, """  mov r9,[rbx-32]
  vpmovdb xmm24,zmm0
  vmovdqu8 [r9+rax]{k7},xmm24""" + LOOPEND + "\n  add [rbx-32],r12")
st(KW['out4'], 1, "  mov r9,[rbx-32]\n  vmovdqu32 [r9+rax*4]{k7},zmm0" + LOOPEND + "\n  lea r9,[r9+r12*4]\n  mov [rbx-32],r9")
st(KW['halt'], 1, LOOPEND + "\n  cmp r12,1" + failif('e') + """
  vmovd edi,xmm0
  test edi,edi
  jz 3f
  mov eax,231
  syscall
3:""")

asm = ".intel_syntax noprefix\n.text\n"
for (cls, d), text in sorted(S.items()):
    text = text.replace('P:', f'P_{cls}_{d}:')
    asm += f"S_{cls}_{d}:\n{text}\nE_{cls}_{d}:\n"
asm += "S_tail:\n  jmp r15\nE_tail:\n"
open(f'{B}/stencils.S', 'w').write(asm)

def assemble(name):
    subprocess.check_call(['as', '-o', f'{B}/{name}.o', f'{B}/{name}.S' if name == 'stencils' else f'{ROOT}/stencils/{name}.S'])
    subprocess.check_call(['objcopy', '-O', 'binary', '-j', '.text', f'{B}/{name}.o', f'{B}/{name}.bin'])
    syms = {}
    for line in subprocess.check_output(['nm', f'{B}/{name}.o'], text=True).splitlines():
        p = line.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    return open(f'{B}/{name}.bin', 'rb').read(), syms

code, syms = assemble('stencils')
sten, sstart, slen, ppos = [], [0] * (NCLS * ND), [0] * (NCLS * ND), [0] * (NCLS * ND)
for (cls, d) in sorted(S):
    a, e = syms[f'S_{cls}_{d}'], syms[f'E_{cls}_{d}']
    tid = cls * ND + d
    sstart[tid], slen[tid] = len(sten), e - a
    blob = bytearray(code[a:e])
    p = syms.get(f'P_{cls}_{d}')
    if p is not None:
        assert blob[p - a - 4:p - a] == bytes.fromhex('44332211'), (cls, d)
        blob[p - a - 4:p - a] = b'\0\0\0\0'
        ppos[tid] = p - a - 4 + 1          # stored +1 so that 0 means "no patch"
    assert bytes.fromhex('44332211') not in blob
    sten += blob
tail = list(code[syms['S_tail']:syms['E_tail']])

stub, ssyms = assemble('stub')
assert ssyms['_start'] == 8 and len(stub) % 64 == 0
ehdr = bytearray(96)
ehdr[0:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
import struct
struct.pack_into('<HHIQQQIHHHHHH', ehdr, 16, 2, 62, 1, VBASE + 112 + ssyms['_start'], 64, 0, 0, 64, 56, 1, 0, 0, 0)
struct.pack_into('<IIQQQ', ehdr, 64, 1, 7, 0, VBASE, VBASE)

T = dict(sten=sten, sstart=sstart, slen=slen, ppos=ppos, tail=tail, stub=list(stub), ehdr=list(ehdr),
         fail=ssyms['fail'])
json.dump(T, open(f'{B}/stencils.json', 'w'))

# ---- data lines for the Strand compiler's own source ----
import random
rnd = random.Random(20261110)
hashw = [rnd.randrange(1, 500000000) * 2 + 1 for _ in range(MAXNAME)]
T['hashw'] = hashw
json.dump(T, open(f'{B}/stencils.json', 'w'))
def data(name, vals, per=24):
    out = []
    vals = list(vals)
    # a data line is one line; keep it one line (the language has no continuation)
    return ' '.join(str(v) for v in vals) + f' ={name}\n'
with open(f'{ROOT}/src/tables.strand', 'w') as f:
    f.write("# GENERATED by tools/mkstencils.py -- do not edit. Stencil bytes come from GNU as.\n")
    f.write('"' + PRELUDE + '" 10 =prelude\n')
    f.write(data('sten', sten)); f.write(data('sstart', sstart)); f.write(data('slen', slen)); f.write(data('ppos', ppos))
    f.write(data('tail', tail)); f.write(data('stub', stub)); f.write(data('ehdr', ehdr))
    f.write(data('delta', [x + 2 for x in DELTA]))      # biased by 2: literals are unsigned
    f.write(data('leadof', LEADOF)); f.write(data('clstab', CLSTAB))
    f.write(data('opctab', [opclass(c) for c in range(128)])); f.write(data('sigtab', [sigil(c) for c in range(128)]))
    f.write(data('defines', DEFINES)); f.write(data('uses', USES)); f.write(data('readsvec', READSVEC)); f.write(data('carry', CARRY))
    f.write(data('hashw', hashw)); f.write(data('pow10', [10 ** i for i in range(MAXDIGITS)]))
    f.write(f"{len(stub)} >Stublen\n")
    for nm, v in [('Kn', K), ('Ncode', NCODE), ('Cdata', DATA), ('Cstr', STR), ('Cnone', NONE), ('Cerr', ERR),
                  ('Cscatter', SCATTER), ('Nd', ND), ('Vbase', VBASE), ('Tabm', TABM), ('Maxname', MAXNAME),
                  ('Maxdig', MAXDIGITS), ('Mult1', RACE_MULT[1]), ('Mult2', RACE_MULT[2])]:
        f.write(f"{v} >{nm}\n")
print(f"stencils: {len(S)} blobs, {len(sten)} bytes; stub {len(stub)} bytes; tables.strand {os.path.getsize(f'{ROOT}/src/tables.strand')} bytes")
