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

# Allocation. Blocks are [64-byte header | data]; header[-8] = address of the free-list head the block
# belongs to. Capacity is a power of two > 4n+127 bytes. Storing tokens allocate in their prologue.
FLOFF = 640
def ALLOC(R):   # R = a block for r12 elements: pop the size class's free list, else bump
    return f"""
  lea rcx,[r12*4+127]
  bsr rcx,rcx
  mov ebp,2
  shlx rbp,rbp,rcx
  lea r10,[rbx+rcx*8-{FLOFF}]
  mov {R},[r10]
  test {R},{R}
  jz 5f
  mov rcx,[{R}]
  mov [r10],rcx
  jmp 6f
5:
  lea {R},[r13+64]
  lea r13,[{R}+rbp]
  cmp r13,[rbx-64]
  jbe 7f
  call r14
7:
  mov [{R}-8],r10
6:"""
# SETUP runs once per line, after r12 = n: zmm27 = n, zmm26 = lane indices. Carries and accumulators
# are initialised by the prologue of the token that uses them.
SETUP = """
  xor eax,eax
  vpbroadcastd zmm27,r12d
  vmovdqa32 zmm26,zmm30
"""
# LOOPHEAD opens every line's loop: the active-lane mask is one vector compare (index < n), no GPR work.
LOOPHEAD = """
  lea r11,[rip+0]
  vpcmpud k7,zmm26,zmm27,1
"""
LOOPEND = """
  vpaddd zmm26,zmm26,dword ptr [rbx-44]{1to16}
  add rax,16
  cmp rax,r12
  jae 1f
  jmp r11
1:"""
RELEASE = """
  mov rsi,[rcx]
  cmp rsi,[rbx-40]
  jb 4f
  mov rdx,[rsi-8]
  mov r9,[rdx]
  mov [rsi],r9
  mov [rdx],rsi
4:"""
def failif(cc_ok):   # continue if cc_ok, else call the fail handler
    return f"""
  j{cc_ok} 2f
  call r14
2:"""
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

S = {}     # (cls, d0) -> body stencil (runs every loop iteration, or after the loop for sink tails)
PRE = {}   # (cls, d0) -> prologue stencil (runs once, before the line's loop)
# Operand placeholders: DISP = slot/pool displacement, DISP8 = the same + 8 (a vector's length field).
DISP8 = '0x1122334c'
def st(cls, d, text, pre=None):
    S[(cls, d)] = text
    if pre is not None: PRE[(cls, d)] = pre

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
    st(KW['scan'], d, scan('vpaddd', '  vpxord zmm25,zmm25,zmm25', 16 + r), pre=f'  vpxord zmm{16+r},zmm{16+r},zmm{16+r}')
    st(KW['scanmax'], d, scan('vpmaxsd', '  vpslld zmm25,zmm31,31', 8 + r), pre=f'  vpslld zmm{8+r},zmm31,31')
    st(KW['prev'], d, f"  valignd zmm24,zmm{r},zmm{16+r},15\n  vmovdqa32 zmm{16+r},zmm{r}\n  vmovdqa32 zmm{r},zmm24",
       pre=f'  vpxord zmm{16+r},zmm{16+r},zmm{16+r}')
    if d <= 7: st(KW['dup'], d, f"  vmovdqa32 zmm{d},zmm{r}")
    if d <= 7: st(KW['idx'], d, f"  vmovdqa32 zmm{d},zmm26")       # lane indices i..i+15
    st(GATHER, d, f"  lea rcx,[rbx+{DISP}]" + bounds(r) + f"""
  mov rsi,[rcx]
  vpxord zmm24,zmm24,zmm24
  vpgatherdd zmm24{{k1}},[rsi+zmm{r}*4]
  vmovdqa32 zmm{r},zmm24""")
    if d <= 7:
        st(VEC, d, f"  mov rsi,[rbx+{DISP}]\n  vmovdqu32 zmm{d}{{k7}}{{z}},[rsi+rax*4]",
           pre=f"  cmp r12,[rbx+{DISP8}]" + failif('e'))       # length check hoisted out of the loop
        st(SCAL, d, f"  vpbroadcastd zmm{d},dword ptr [rbx+{DISP}]")
        st(LEN, d, f"  vpbroadcastd zmm{d},dword ptr [rbx+{DISP8}]")

st(VECLEAD, 0, LOOPHEAD + f"  mov rsi,[rbx+{DISP}]\n  vmovdqu32 zmm0{{k7}}{{z}},[rsi+rax*4]", pre=f"  mov r12,[rbx+{DISP8}]\n" + SETUP)
st(SCALLEAD, 0, LOOPHEAD + f"  vpbroadcastd zmm0,dword ptr [rbx+{DISP}]", pre="  mov r12d,1\n" + SETUP)
st(LENLEAD, 0, LOOPHEAD + f"  vpbroadcastd zmm0,dword ptr [rbx+{DISP8}]", pre="  mov r12d,1\n" + SETUP)
st(IOTA, 0, LOOPHEAD + "  vmovdqa32 zmm0,zmm26", pre=f"  mov r12d,dword ptr [rbx+{DISP}]\n  test r12d,r12d" + failif('ns') + SETUP)
st(KW['input'], 0, LOOPHEAD + "  mov rsi,[rbx-16]\n  vpmovzxbd zmm0{k7}{z},xmmword ptr [rsi+rax]", pre="  mov r12,[rbx-8]\n" + SETUP)
st(STOREV, 1, pre=ALLOC('rdi'), text="  vmovdqu32 [rdi+rax*4]{k7},zmm0" + LOOPEND + f"""
  lea rcx,[rbx+{DISP}]""" + RELEASE + """
  mov [rcx],rdi
  mov [rcx+8],r12""")
st(STORES, 1, LOOPEND + "\n  cmp r12,1" + failif('e') + f"\n  vmovd dword ptr [rbx+{DISP}],xmm0")
st(SUM, 1, "  vpaddd zmm28{k7},zmm28,zmm0" + LOOPEND + hred(28, 'vpaddd') + f"\n  mov dword ptr [rbx+{DISP}],eax", pre="  vpxord zmm28,zmm28,zmm28")
st(MAX, 1, "  vpmaxsd zmm29{k7},zmm29,zmm0" + LOOPEND + hred(29, 'vpmaxsd') + f"\n  mov dword ptr [rbx+{DISP}],eax", pre="  vpslld zmm29,zmm31,31")
st(COMPRESS, 2, pre=ALLOC('rdi') + "\n  mov r8,rdi", text="""  vptestmd k1{k7},zmm1,zmm1
  vpcompressd zmm24{k1}{z},zmm0
  vmovdqu32 [r8],zmm24
  kmovw ecx,k1
  popcnt ecx,ecx
  lea r8,[r8+rcx*4]""" + LOOPEND + f"""
  lea rcx,[rbx+{DISP}]""" + RELEASE + """
  mov [rcx],rdi
  sub r8,rdi
  shr r8,2
  mov [rcx+8],r8""")
# scatter: a negative index is a masked-off lane (no write); an index >= len is a failure
st(SCATTER, 2, f"  lea rcx,[rbx+{DISP}]" + """
  vpcmpud k1{k7},zmm1,dword ptr [rcx+8]{1to16},1
  vpmovd2m k2,zmm1
  korw k2,k2,k1
  kandnw k2,k2,k7
  kortestw k2,k2""" + failif('z') + """
  mov rsi,[rcx]
  vpscatterdd [rsi+zmm1*4]{k1},zmm0""" + LOOPEND)
st(KW['out'], 1, """  mov rsi,[rbx-32]
  vpmovdb xmm24,zmm0
  vmovdqu8 [rsi+rax]{k7},xmm24""" + LOOPEND + "\n  add [rbx-32],r12",
   pre="  mov rsi,[rbx-32]\n  add rsi,r12\n  cmp rsi,[rbx-56]" + failif('be'))
st(KW['out4'], 1, "  mov rsi,[rbx-32]\n  vmovdqu32 [rsi+rax*4]{k7},zmm0" + LOOPEND + "\n  lea rsi,[rsi+r12*4]\n  mov [rbx-32],rsi",
   pre="  mov rsi,[rbx-32]\n  lea rsi,[rsi+r12*4]\n  cmp rsi,[rbx-56]" + failif('be'))
st(KW['end'], 1, LOOPEND)
# tee stores: keep the value on the stack and keep the loop going, so one line can define many vectors
for d in range(1, 9):
    st(TEE, d, f"  mov rsi,[rbx+{DISP}]\n  vmovdqu32 [rsi+rax*4]{{k7}},zmm{d-1}",
       pre=ALLOC('rsi') + f"\n  lea rcx,[rbx+{DISP}]\n  mov [rcx],rsi\n  mov [rcx+8],r12")
for d in range(3, 9):    # never at depth 2: a line must not end on a tee
    st(TEEC, d, f"""  lea rcx,[rbx+{DISP}]
  vptestmd k1{{k7}},zmm{d-1},zmm{d-1}
  vpcompressd zmm24{{k1}}{{z}},zmm{d-2}
  mov rsi,[rcx]
  mov rdx,[rcx+8]
  vmovdqu32 [rsi+rdx*4],zmm24
  kmovw esi,k1
  popcnt esi,esi
  add [rcx+8],rsi""", pre=ALLOC('rsi') + f"\n  lea rcx,[rbx+{DISP}]\n  mov [rcx],rsi\n  mov qword ptr [rcx+8],0")
st(KW['halt'], 1, LOOPEND + "\n  cmp r12,1" + failif('e') + """
  vmovd edi,xmm0
  test edi,edi
  jz 3f
  mov eax,231
  syscall
3:""")

asm = ".intel_syntax noprefix\n.text\n"
for kind, D in (('S', S), ('Q', PRE)):
    for (cls, d), text in sorted(D.items()):
        asm += f"{kind}_{cls}_{d}:\n{text}\nE{kind}_{cls}_{d}:\n"
asm += "S_tail:\n  jmp r15\nES_tail:\n"
# release stencil: emitted after the line holding a vector's last occurrence
asm += f"S_rel:\n  lea rcx,[rbx+{DISP}]" + RELEASE + "\n  mov qword ptr [rcx],0\n  mov qword ptr [rcx+8],0\nES_rel:\n"
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
PH = {bytes.fromhex('44332211'): 0, bytes.fromhex('4c332211'): 8}
def extract(blob):
    """find the (at most one) operand placeholder; return (bytes with it zeroed, pos+1 or 0, addend)"""
    blob = bytearray(blob); pos, add = 0, 0
    for ph, addend in PH.items():
        k = blob.find(ph)
        if k >= 0:
            assert pos == 0 and blob.find(ph, k + 1) < 0, 'one operand per stencil'
            pos, add = k + 1, addend
            blob[k:k + 4] = bytes(4)
    return blob, pos, add
NT_ = NCLS * ND
sten = []
tabs = {n: [0] * NT_ for n in ('sstart', 'slen', 'ppos', 'padd', 'qstart', 'qlen', 'qpos', 'qadd')}
for kind, D, names in (('S', S, ('sstart', 'slen', 'ppos', 'padd')), ('Q', PRE, ('qstart', 'qlen', 'qpos', 'qadd'))):
    for (cls, d) in sorted(D):
        blob, pos, add = extract(code[syms[f'{kind}_{cls}_{d}']:syms[f'E{kind}_{cls}_{d}']])
        tid = cls * ND + d
        for n, v in zip(names, (len(sten), len(blob), pos, add)): tabs[n][tid] = v
        sten += blob
tail = list(code[syms['S_tail']:syms['ES_tail']])
rel, relp, _ = extract(code[syms['S_rel']:syms['ES_rel']])
rels, rell = len(sten), len(rel)
sten += rel

stub, ssyms = assemble('stub')
assert ssyms['_start'] == 8 and len(stub) % 64 == 0
ehdr = bytearray(96)
ehdr[0:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
import struct
struct.pack_into('<HHIQQQIHHHHHH', ehdr, 16, 2, 62, 1, VBASE + 112 + ssyms['_start'], 64, 0, 0, 64, 56, 1, 0, 0, 0)
struct.pack_into('<IIQQQ', ehdr, 64, 1, 7, 0, VBASE, VBASE)

T = dict(rels=rels, rell=rell, relp=relp, sten=sten, tail=tail, **tabs, stub=list(stub), ehdr=list(ehdr),
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
    f.write(data('sten', sten))
    for n in tabs: f.write(data(n, tabs[n]))
    f.write(data('tail', tail)); f.write(data('stub', stub)); f.write(data('ehdr', ehdr))
    f.write(data('delta', [x + 2 for x in DELTA]))      # biased by 2: literals are unsigned
    f.write(data('leadof', LEADOF)); f.write(data('clstab', CLSTAB))
    f.write(data('opctab', [opclass(c) for c in range(128)])); f.write(data('sigtab', [sigil(c) for c in range(128)]))
    f.write(data('defines', DEFINES)); f.write(data('uses', USES)); f.write(data('readsvec', READSVEC)); f.write(data('carry', CARRY))
    f.write(data('pow10', [10 ** i for i in range(MAXDIGITS)]))
    f.write(f"{len(stub)} >Stublen\n")
    for nm, v in [('Rels', rels), ('Rell', rell), ('Relp', relp), ('Kn', K), ('Ncode', NCODE), ('Cdata', DATA), ('Cstr', STR), ('Cnone', NONE), ('Cerr', ERR),
                  ('Cscatter', SCATTER), ('Ctee', TEE), ('Cteec', TEEC), ('Cvec', VEC), ('Clen', LEN), ('Nsig5', 5), ('Nd', ND), ('Vbase', VBASE), ('Maxname', MAXNAME), ('Maxdig', MAXDIGITS),
                  ('Ha1', HA1), ('Hb1', HB1), ('Hs1', HS1), ('Ha2', HA2), ('Hb2', HB2), ('Hs2', HS2)]\
                 + [('Ru%d' % r, RACE_U[r]) for r in range(ROUNDS)] + [('Rv%d' % r, RACE_V[r]) for r in range(ROUNDS)]:
        f.write(f"{v} >{nm}\n")
print(f"stencils: {len(S)} bodies + {len(PRE)} prologues, {len(sten)} bytes; stub {len(stub)} bytes; tables.strand {os.path.getsize(f'{ROOT}/src/tables.strand')} bytes")
