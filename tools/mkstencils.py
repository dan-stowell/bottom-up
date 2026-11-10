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
  mov edx,2
  shlx rdx,rdx,rcx
  lea r10,[rbx+rcx*8-{FLOFF}]
  mov {R},[r10]
  test {R},{R}
  jz 5f
  mov rcx,[{R}]
  mov [r10],rcx
  jmp 6f
5:
  lea {R},[r13+64]
  lea r13,[{R}+rdx]
  cmp r13,[rbx-64]
  jbe 7f
  call r14
7:
  mov [{R}-8],r10
6:"""
# SETUP runs once per line, after r12 = n: zmm27 = n, zmm26 = lane indices. Carries and accumulators
# are initialised by the prologue of the token that uses them.
VETO = "  mov dword ptr [rbx-672],0"      # this token cannot be split across threads
SETUP = """
  mov dword ptr [rbx-672],1
  mov dword ptr [rbx-680],0
  mov dword ptr [rbx-704],0
  mov dword ptr [rbx-708],16
  mov dword ptr [rbx-712],0
  xor eax,eax
  vpbroadcastd zmm27,r12d
  vmovdqa32 zmm26,zmm30
"""
# LOOPHEAD opens every line's loop: the active-lane mask is one vector compare (index < n), no GPR work.
# k7 masks the lanes that exist; k6 = k7 & k5 masks the lanes whose results are *kept*. The pool
# clears k5 for a warm-up chunk, which recomputes `prev` carries without storing anything.
# The line votes at run time: the lead's prologue says "splittable", any token that is not
# (running sums, compress cursors, scatters) vetoes in its own prologue. A big splittable line calls
# the pool, which runs most of it on all cores and returns with rax = where the serial tail starts.
LOOPHEAD = """
  mov rbp,r12
  lea r11,[rip+7f]
  cmp dword ptr [rbx-672],0
  je 7f
  cmp r12,[rbx-696]
  jb 7f
  call [rbx-656]
7:
  vpcmpud k7,zmm26,zmm27,1
  kandw k6,k7,k5
"""
LOOPEND = """
  vpaddd zmm26,zmm26,dword ptr [rbx-44]{1to16}
  add rax,16
  cmp rax,rbp
  jae 1f
  jmp r11
1:
  test r15,r15
  jz 8f
  jmp r15
8:"""
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
DISPL = '0x11223384'                # the same + 64: offset of a chunk-local vector from rsp
DISP16 = '0x11223354'               # the same + 16: a vector's byte-table pointer
DISP24 = '0x1122335c'               # the same + 24: a vector's packed automaton table pointer
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
    st(KW['scan'], d, scan('vpaddd', '  vpxord zmm25,zmm25,zmm25', 16 + r), pre=f'  vpxord zmm{16+r},zmm{16+r},zmm{16+r}\n  or dword ptr [rbx-712],{1 << r}\n  mov dword ptr [rbx-680],0')
    st(KW['scanmax'], d, scan('vpmaxsd', '  vpslld zmm25,zmm31,31', 8 + r), pre=f'  vpslld zmm{8+r},zmm31,31\n  or dword ptr [rbx-712],{1 << (8 + r)}\n  mov dword ptr [rbx-680],0')
    st(KW['prev'], d, f"  valignd zmm24,zmm{r},zmm{16+r},15\n  vmovdqa32 zmm{16+r},zmm{r}\n  vmovdqa32 zmm{r},zmm24",
       pre=f'  vpxord zmm{16+r},zmm{16+r},zmm{16+r}\n  or dword ptr [rbx-712],{1 << (16 + r)}\n  mov dword ptr [rbx-680],0')
    if d <= 7: st(KW['dup'], d, f"  vmovdqa32 zmm{d},zmm{r}")
    if d <= 7: st(KW['idx'], d, f"  vmovdqa32 zmm{d},zmm26")       # lane indices i..i+15
    st(GATHER, d, f"  lea rcx,[rbx+{DISP}]" + bounds(r) + f"""
  mov rsi,[rcx]
  vpxord zmm24,zmm24,zmm24
  vpgatherdd zmm24{{k1}},[rsi+zmm{r}*4]
  vmovdqa32 zmm{r},zmm24""")
    # reading a byte vector from a dword line: `i [x` one byte, `i {x` four (bytes past the end are 0).
    # Blocks have at least 127 bytes of slack, so the gather itself never leaves the block.
    st(GATHERB, d, f"  lea rcx,[rbx+{DISP}]" + bounds(r) + f"""
  mov rsi,[rcx]
  vpxord zmm24,zmm24,zmm24
  vpgatherdd zmm24{{k1}},[rsi+zmm{r}*1]
  vpslld zmm24,zmm24,24
  vpsrld zmm{r},zmm24,24""")
    st(GATHER4, d, f"  lea rcx,[rbx+{DISP}]" + bounds(r) + f"""
  mov rsi,[rcx]
  vpxord zmm24,zmm24,zmm24
  vpgatherdd zmm24{{k1}},[rsi+zmm{r}*1]
  vpbroadcastd zmm25,dword ptr [rcx+8]
  vpsubd zmm25,zmm25,zmm{r}
  vpslld zmm{r},zmm31,2
  vpminud zmm25,zmm25,zmm{r}
  vpslld zmm25,zmm25,3
  vpslld zmm{r},zmm{r},3
  vpsubd zmm25,zmm{r},zmm25
  vpsllvd zmm24,zmm24,zmm25
  vpsrlvd zmm{r},zmm24,zmm25""")
    if d <= 7:
        st(VEC, d, f"  mov rsi,[rbx+{DISP}]\n  vmovdqu32 zmm{d}{{k7}}{{z}},[rsi+rax*4]",
           pre=f"  cmp r12,[rbx+{DISP8}]" + failif('e'))       # length check hoisted out of the loop
        st(VECT, d, f"  mov rsi,[rbx+{DISP}]\n  vmovdqu32 zmm{d}{{k7}}{{z}},[rsi+rax*4]",
           pre=f"  cmp r12,[rbx+{DISP8}]" + failif('e') + "\n" + VETO)
        st(SCAL, d, f"  vpbroadcastd zmm{d},dword ptr [rbx+{DISP}]")
        st(LEN, d, f"  vpbroadcastd zmm{d},dword ptr [rbx+{DISP8}]")

st(VECLEAD, 0, LOOPHEAD + f"  mov rsi,[rbx+{DISP}]\n  vmovdqu32 zmm0{{k7}}{{z}},[rsi+rax*4]", pre=f"  mov r12,[rbx+{DISP8}]\n" + SETUP)
st(SCALLEAD, 0, LOOPHEAD + f"  vpbroadcastd zmm0,dword ptr [rbx+{DISP}]", pre="  mov r12d,1\n" + SETUP)
st(LENLEAD, 0, LOOPHEAD + f"  vpbroadcastd zmm0,dword ptr [rbx+{DISP8}]", pre="  mov r12d,1\n" + SETUP)
st(IOTA, 0, LOOPHEAD + "  vmovdqa32 zmm0,zmm26", pre=f"  mov r12d,dword ptr [rbx+{DISP}]\n  test r12d,r12d" + failif('ns') + SETUP)
st(KW['input'], 0, LOOPHEAD + "  mov rsi,[rbx-16]\n  vpmovzxbd zmm0{k7}{z},xmmword ptr [rsi+rax]", pre="  mov r12,[rbx-8]\n" + SETUP)
# stdin as little-endian dwords (a trailing partial dword is ignored)
st(KW['input4'], 0, LOOPHEAD + "  mov rsi,[rbx-16]\n  vmovdqu32 zmm0{k7}{z},[rsi+rax*4]", pre="  mov r12,[rbx-8]\n  shr r12,2\n" + SETUP)
st(STOREV, 1, pre=ALLOC('rdi'), text="  vmovdqu32 [rdi+rax*4]{k6},zmm0" + LOOPEND + f"""
  lea rcx,[rbx+{DISP}]""" + RELEASE + """
  mov [rcx],rdi
  mov [rcx+8],r12""")
st(STORES, 1, LOOPEND + "\n  cmp r12,1" + failif('e') + f"\n  vmovd dword ptr [rbx+{DISP}],xmm0")
st(SUM, 1, "  vpaddd zmm28{k6},zmm28,zmm0" + LOOPEND + hred(28, 'vpaddd') + f"\n  mov dword ptr [rbx+{DISP}],eax", pre="  vpxord zmm28,zmm28,zmm28")
st(MAX, 1, "  vpmaxsd zmm29{k6},zmm29,zmm0" + LOOPEND + hred(29, 'vpmaxsd') + f"\n  mov dword ptr [rbx+{DISP}],eax", pre="  vpslld zmm29,zmm31,31")
st(COMPRESS, 2, pre=ALLOC('rdi') + "\n  mov r8,rdi\n" + VETO, text="""  vptestmd k1{k7},zmm1,zmm1
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
st(SCATTER, 2, pre=VETO, text=f"  lea rcx,[rbx+{DISP}]" + """
  vpcmpud k1{k7},zmm1,dword ptr [rcx+8]{1to16},1
  vpmovd2m k2,zmm1
  korw k2,k2,k1
  kandnw k2,k2,k7
  kortestw k2,k2""" + failif('z') + """
  mov rsi,[rcx]
  vpscatterdd [rsi+zmm1*4]{k1},zmm0""" + LOOPEND)
# Output is streamed: r8 walks a 256 KiB buffer that is flushed (a call into the stub) when full.
# After the loop the cursor is corrected for the lanes of the last chunk that were masked off.
def outsink(store, step, per):
    return (f"""  cmp r8,[rbx-56]
  jb 3f
  call [rbx-648]
3:
{store}
  add r8,{step}""" + LOOPEND + f"""
  lea rcx,[r12+15]
  shr rcx,4
  cmp rcx,1
  adc rcx,0
  shl rcx,4
  sub rcx,r12
  imul rcx,rcx,{per}
  sub r8,rcx
  mov [rbx-32],r8""")
st(KW['out'], 1, outsink("  vpmovdb xmm24,zmm0\n  vmovdqu8 [r8]{k6},xmm24", 16, 1), pre="  mov r8,[rbx-32]\n  mov dword ptr [rbx-704],1")
st(KW['out4'], 1, outsink("  vmovdqu32 [r8]{k6},zmm0", 64, 4), pre="  mov r8,[rbx-32]\n  mov dword ptr [rbx-704],4")
# streaming filter: `vals mask outif` appends the selected values to stdout, nothing is materialised
def outifsink(narrow, store, adv):
    return (f"""  cmp r8,[rbx-56]
  jb 3f
  call [rbx-648]
3:
  vptestmd k1{{k6}},zmm1,zmm1
  vpcompressd zmm24{{k1}}{{z}},zmm0
{narrow}  {store}
  kmovw ecx,k1
  popcnt ecx,ecx
  {adv}""" + LOOPEND + "\n  mov [rbx-32],r8")
st(KW['outif'], 2, outifsink("  vpmovdb xmm24,zmm24\n", "vmovdqu8 [r8],xmm24", "add r8,rcx"), pre="  mov r8,[rbx-32]\n  mov dword ptr [rbx-704],1")
st(KW['outif4'], 2, outifsink("", "vmovdqu32 [r8],zmm24", "lea r8,[r8+rcx*4]"), pre="  mov r8,[rbx-32]\n  mov dword ptr [rbx-704],4")
st(KW['end'], 1, LOOPEND)
# tee stores: keep the value on the stack and keep the loop going, so one line can define many vectors
for d in range(1, 9):
    st(TEE, d, f"  mov rsi,[rbx+{DISP}]\n  vmovdqu32 [rsi+rax*4]{{k6}},zmm{d-1}",
       pre=ALLOC('rsi') + f"\n  lea rcx,[rbx+{DISP}]\n  mov [rcx],rsi\n  mov [rcx+8],r12")
for d in range(1, 9):       # chunk-local vectors: the slot itself holds the current chunk
    st(TEEL, d, f"  vmovdqu32 [rsp+{DISPL}],zmm{d-1}")
    if d <= 7:
        st(VECL, d, f"  vmovdqu32 zmm{d},[rsp+{DISPL}]")
        st(LENL, d, f"  vmovdqa32 zmm{d},zmm27")
for d in range(3, 9):    # never at depth 2: a line must not end on a tee
    st(TEEC, d, f"""  lea rcx,[rbx+{DISP}]
  vptestmd k1{{k7}},zmm{d-1},zmm{d-1}
  vpcompressd zmm24{{k1}}{{z}},zmm{d-2}
  mov rsi,[rcx]
  mov rdx,[rcx+8]
  vmovdqu32 [rsi+rdx*4],zmm24
  kmovw esi,k1
  popcnt esi,esi
  add [rcx+8],rsi""", pre=ALLOC('rsi') + f"\n  lea rcx,[rbx+{DISP}]\n  mov [rcx],rsi\n  mov qword ptr [rcx+8],0\n" + VETO)
st(KW['halt'], 1, LOOPEND + "\n  cmp r12,1" + failif('e') + """
  vmovd edi,xmm0
  test edi,edi
  jz 3f
  test dil,dil                      # the exit status is the low byte; never report success
  jnz 4f
  mov edi,1
4:
  mov eax,231
  syscall
3:""")

# ------------------------------------------------------------------ byte lines: 64 u8 lanes per chunk
# Same loop skeleton, stepping 64. The active-lane mask comes from the remaining count (bzhi);
# zmm26 (lane indices, unused here) serves as a third temporary.
BKW = lambda k: BK + KW[k]
SETUPB = """
  mov dword ptr [rbx-672],1
  mov dword ptr [rbx-680],0
  mov dword ptr [rbx-704],0
  mov dword ptr [rbx-708],64
  mov dword ptr [rbx-712],0
  xor eax,eax
"""
LOOPHEADB = """
  mov rbp,r12
  lea r11,[rip+7f]
  cmp dword ptr [rbx-672],0
  je 7f
  cmp r12,[rbx-696]
  jb 7f
  call [rbx-656]
7:
  mov rcx,r12
  sub rcx,rax
  mov edx,64
  cmp rcx,rdx
  cmova rcx,rdx
  mov rdx,-1
  bzhi rdx,rdx,rcx
  kmovq k7,rdx
  kandq k6,k7,k5
"""
LOOPENDB = """
  add rax,64
  cmp rax,rbp
  jae 1f
  jmp r11
1:
  test r15,r15
  jz 8f
  jmp r15
8:"""
FLUSHCHK = "  cmp r8,[rbx-56]\n  jb 3f\n  call [rbx-648]\n3:\n"
st(BKW('bytes'), 0, LOOPHEADB + "  mov rsi,[rbx-16]\n  vmovdqu8 zmm0{k7}{z},[rsi+rax]", pre="  mov r12,[rbx-8]\n" + SETUPB)
BBIN = {'+': 'vpaddb', '-': 'vpsubb', '&': 'vpandd', '|': 'vpord', '^': 'vpxord', 'min': 'vpminub', 'max': 'vpmaxub'}
for d in range(2, 9):
    a, b = d - 2, d - 1
    for k, ins in BBIN.items(): st(BKW(k), d, f"  {ins} zmm{a},zmm{a},zmm{b}")
    for k, imm in CMP.items(): st(BKW(k), d, f"  vpcmpub k1,zmm{a},zmm{b},{imm}\n  vpmovm2b zmm{a},k1\n  vpabsb zmm{a},zmm{a}")
    st(BKW('*'), d, f"""  vpmullw zmm24,zmm{a},zmm{b}
  vpsrlw zmm25,zmm{a},8
  vpsrlw zmm26,zmm{b},8
  vpmullw zmm25,zmm25,zmm26
  vpsllw zmm25,zmm25,8
  vpsllw zmm24,zmm24,8
  vpsrlw zmm24,zmm24,8
  vpord zmm{a},zmm24,zmm25""")
    st(BKW('drop'), d, "  nop")
    st(BKW('swap'), d, f"  vmovdqa32 zmm24,zmm{a}\n  vmovdqa32 zmm{a},zmm{b}\n  vmovdqa32 zmm{b},zmm24")
    if d <= 7: st(BKW('over'), d, f"  vmovdqa32 zmm{d},zmm{a}")
for d in range(3, 9):
    c, a, b = d - 3, d - 2, d - 1
    st(BKW('sel'), d, f"  vptestmb k1,zmm{c},zmm{c}\n  vpblendmb zmm{c}{{k1}},zmm{b},zmm{a}")
for d in range(1, 9):
    r = d - 1
    st(BKW('prev'), d, f"""  valignq zmm24,zmm{r},zmm{16+r},7
  vpsllq zmm25,zmm{r},8
  vpsrlq zmm24,zmm24,56
  vmovdqa64 zmm{16+r},zmm{r}
  vporq zmm{r},zmm25,zmm24""", pre=f'  vpxord zmm{16+r},zmm{16+r},zmm{16+r}\n  or dword ptr [rbx-712],{1 << (16 + r)}\n  mov dword ptr [rbx-680],0')
    # 256-entry table lookup: the prologue narrows the i32 table to bytes once; the body is two vpermi2b
    st(TABLEB, d, f"""  lea rcx,[rbx+{DISP}]
  mov rsi,[rcx+16]
  vmovdqa64 zmm24,zmm{r}
  vmovdqu8 zmm25,[rsi]
  vpermi2b zmm24,zmm25,[rsi+64]
  vmovdqa64 zmm26,zmm{r}
  vmovdqu8 zmm25,[rsi+128]
  vpermi2b zmm26,zmm25,[rsi+192]
  vpmovb2m k1,zmm{r}
  vpblendmb zmm{r}{{k1}},zmm24,zmm26""", pre=f"""  lea rcx,[rbx+{DISP}]
  cmp qword ptr [rcx+8],256""" + failif('ae') + """
  mov rsi,[rcx]
  mov rdx,r13
  add r13,256
  cmp r13,[rbx-64]
  jbe 7f
  call r14
7:
  mov [rcx+16],rdx
  xor r10d,r10d
5:
  vmovdqu32 zmm24,[rsi+r10*4]
  vpmovdb [rdx+r10],zmm24
  add r10,16
  cmp r10,256
  jb 5b""")
    if d <= 7:
        st(BKW('dup'), d, f"  vmovdqa32 zmm{d},zmm{r}")
        st(SCALB, d, f"  vpbroadcastb zmm{d},byte ptr [rbx+{DISP}]")
# ---- automata: `x /t` -- the state of a 4-state automaton after each byte, as a scan of functions.
# A transition function is 4 bytes (next state from states 0..3), so one fits in a dword and composing
# two is one vpshufb. The 64 bytes of a chunk are taken as 16 groups of 4. Register t (zmm8+t) gets the
# functions of bytes 4g+t, one dword per group g: `vpermd` looks them up by class. Level 1 composes
# across the four registers (3 vpshufb for all 16 groups); level 2 is a Kogge-Stone scan over the 16
# group functions in one register; the carry (the function from the start of the line, kept xor
# identity so that zero means "nothing read yet") is composed in last. vpshufb indexes within
# 128-bit lanes, so indices carry their dword's offset 4j ("pre-offset"); a function shifted by one
# or two dwords is fixed up by adding 4 or 8, a shift by 4 or 8 dwords needs nothing.
# In the pool's dry pass only the carry matters. If no later token on the line has a carry of its own
# (each carry user's prologue clears [rbx-680]; this one then writes its id there), the pool copies
# that id to [rbx-676] for the duration of the dry pass and the body leaves the loop right here.
# Constants live in the stub: [rbx-960] offsets, [rbx-896] identity|offsets, [rbx-832..] dwords and masks.
for d in range(1, 9):
    r = d - 1; C = 16 + r
    st(FSMB, d, f"""  mov rsi,[rbx+{DISP24}]
  vmovdqu64 zmm13,[rsi]
  vpermd zmm8,zmm{r},zmm13
  vpsrld zmm24,zmm{r},8
  vpermd zmm9,zmm24,zmm13
  vpsrld zmm24,zmm{r},16
  vpermd zmm10,zmm24,zmm13
  vpsrld zmm24,zmm{r},24
  vpermd zmm11,zmm24,zmm13
  vmovdqu64 zmm26,[rbx-960]
  vmovdqu64 zmm12,[rbx-896]
  vpord zmm8,zmm8,zmm26
  vpshufb zmm9,zmm9,zmm8
  vpord zmm9,zmm9,zmm26
  vpshufb zmm10,zmm10,zmm9
  vpord zmm10,zmm10,zmm26
  vpshufb zmm11,zmm11,zmm10
  vpord zmm11,zmm11,zmm26
  valignd zmm24,zmm11,zmm12,15
  vpaddd zmm24,zmm24,dword ptr [rbx-832]{{1to16}}
  vpshufb zmm25,zmm11,zmm24
  valignd zmm24,zmm25,zmm12,14
  vpaddd zmm24,zmm24,dword ptr [rbx-828]{{1to16}}
  vpshufb zmm25,zmm25,zmm24
  valignd zmm24,zmm25,zmm12,12
  vpshufb zmm25,zmm25,zmm24
  valignd zmm24,zmm25,zmm12,8
  vpshufb zmm25,zmm25,zmm24
  vpxord zmm24,zmm{C},zmm12
  vpshufb zmm25,zmm25,zmm24
  valignd zmm24,zmm25,zmm24,15
  vpaddd zmm24,zmm24,dword ptr [rbx-832]{{1to16}}
  valignd zmm25,zmm25,zmm25,15
  vpbroadcastd zmm{C},xmm25
  vpxord zmm{C},zmm{C},dword ptr [rbx-820]{{1to16}}
  cmp dword ptr [rbx-676],{r + 1}
  jne 9f
  add rax,64
  cmp rax,rbp
  jae 1f
  jmp r11
1:
  jmp r15
9:
  vpshufb zmm24,zmm24,zmm26
  kmovq k1,[rbx-816]
  kshiftlq k2,k1,1
  kshiftlq k3,k1,2
  kshiftlq k4,k1,3
  vpshufb zmm{r}{{k1}}{{z}},zmm8,zmm24
  vpshufb zmm{r}{{k2}},zmm9,zmm24
  vpshufb zmm{r}{{k3}},zmm10,zmm24
  vpshufb zmm{r}{{k4}},zmm11,zmm24
  vpandd zmm{r},zmm{r},dword ptr [rbx-824]{{1to16}}""", pre=f"""  lea rcx,[rbx+{DISP}]
  mov rsi,[rcx]
  mov rdx,[rcx+8]
  mov r10d,64
  cmp rdx,r10
  cmova rdx,r10
  mov r10,-1
  bzhi r10,r10,rdx
  kmovq k1,r10
  mov rdx,r13
  add r13,64
  cmp r13,[rbx-64]
  jbe 7f
  call r14
7:
  mov [rcx+24],rdx
  vmovdqu32 zmm24{{k1}}{{z}},[rsi]
  vpmovdb [rdx],zmm24
  kshiftrq k1,k1,16
  vmovdqu32 zmm24{{k1}}{{z}},[rsi+64]
  vpmovdb [rdx+16],zmm24
  kshiftrq k1,k1,16
  vmovdqu32 zmm24{{k1}}{{z}},[rsi+128]
  vpmovdb [rdx+32],zmm24
  kshiftrq k1,k1,16
  vmovdqu32 zmm24{{k1}}{{z}},[rsi+192]
  vpmovdb [rdx+48],zmm24
  vmovdqu8 zmm24,[rdx]
  vpandd zmm24,zmm24,dword ptr [rbx-824]{{1to16}}
  vmovdqu8 [rdx],zmm24
  vpxord zmm{C},zmm{C},zmm{C}
  or dword ptr [rbx-712],{1 << (24 + r)}
  mov dword ptr [rbx-680],{r + 1}""")
st(SUMB, 1, """  vmovdqu8 zmm24{k6}{z},zmm0
  vpxord zmm25,zmm25,zmm25
  vpsadbw zmm24,zmm24,zmm25
  vpaddd zmm28,zmm28,zmm24""" + LOOPENDB + hred(28, 'vpaddd') + f"\n  mov dword ptr [rbx+{DISP}],eax", pre="  vpxord zmm28,zmm28,zmm28")
st(BKW('out'), 1, FLUSHCHK + "  vmovdqu8 [r8]{k6},zmm0\n  add r8,64" + LOOPENDB + """
  lea rcx,[r12+63]
  shr rcx,6
  cmp rcx,1
  adc rcx,0
  shl rcx,6
  sub rcx,r12
  sub r8,rcx
  mov [rbx-32],r8""", pre="  mov r8,[rbx-32]\n  mov dword ptr [rbx-704],1")
st(BKW('outif'), 2, FLUSHCHK + """  vptestmb k1{k6},zmm1,zmm1
  vpcompressb zmm24{k1}{z},zmm0
  vmovdqu8 [r8],zmm24
  kmovq rcx,k1
  popcnt rcx,rcx
  add r8,rcx""" + LOOPENDB + "\n  mov [rbx-32],r8", pre="  mov r8,[rbx-32]\n  mov dword ptr [rbx-704],1")
st(BKW('end'), 1, LOOPENDB)
# ---- the bridge to dword lines (spec.py). `>x` on a byte line keeps the bytes as a vector.
st(STOREVB, 1, pre=ALLOC('rdi'), text="  vmovdqu8 [rdi+rax]{k6},zmm0" + LOOPENDB + f"""
  lea rcx,[rbx+{DISP}]""" + RELEASE + """
  mov [rcx],rdi
  mov [rcx+8],r12""")
for d in range(1, 9):     # ':x' on a byte line, when x is read after the line: the same store, as a tee
    st(TEEBX, d, f"  mov rsi,[rbx+{DISP}]\n  vmovdqu8 [rsi+rax]{{k6}},zmm{d-1}",
       pre=ALLOC('rsi') + f"\n  lea rcx,[rbx+{DISP}]\n  mov [rcx],rsi\n  mov [rcx+8],r12")
# `m %x` on a byte line appends the positions of the nonzero lanes to the dword vector x: four
# compresses of the lane indices, 16 lanes each. Like the dword '%' tee it keeps its cursor in the
# slot and keeps the line on one thread.
for d in range(2, 9):    # never at depth 1: a line must not end on a tee
    r = d - 1
    st(WHEREB, d, f"""  lea rcx,[rbx+{DISP}]
  vptestmb k1{{k7}},zmm{r},zmm{r}
  mov rsi,[rcx]
  mov rdx,[rcx+8]
  vpbroadcastd zmm24,eax
  vpaddd zmm24,zmm24,zmm30""" + """
  vpcompressd zmm25{k1}{z},zmm24
  vmovdqu32 [rsi+rdx*4],zmm25
  kmovw r10d,k1
  popcnt r10d,r10d
  add rdx,r10
  kshiftrq k1,k1,16
  vpaddd zmm24,zmm24,dword ptr [rbx-44]{1to16}""" * 4 + """
  mov [rcx+8],rdx""", pre=ALLOC('rsi') + f"\n  lea rcx,[rbx+{DISP}]\n  mov [rcx],rsi\n  mov qword ptr [rcx+8],0\n" + VETO)

asm = ".intel_syntax noprefix\n.text\n"
for kind, D in (('S', S), ('Q', PRE)):
    for (cls, d), text in sorted(D.items()):
        asm += f"{kind}_{cls}_{d}:\n{text}\nE{kind}_{cls}_{d}:\n"
asm += "S_tail:\n  jmp [rbx-688]\nES_tail:\n"
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
PH = {bytes.fromhex('44332211'): 0, bytes.fromhex('4c332211'): 8, bytes.fromhex('84332211'): 64, bytes.fromhex('54332211'): 16, bytes.fromhex('5c332211'): 24}
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
    f.write(data('leadof', LEADOF)); f.write(data('localof', LOCALOF)); f.write(data('byteof', BYTEOF)); f.write(data('istee', ISTEE)); f.write(data('reread', REREAD)); f.write(data('clstab', CLSTAB))
    f.write(data('opctab', [opclass(c) for c in range(128)])); f.write(data('sigtab', [sigil(c) for c in range(128)]))
    f.write(data('isteec', ISTEEC)); f.write(data('vtype', VTYPE)); f.write(data('defines', DEFINES)); f.write(data('uses', USES)); f.write(data('readsvec', READSVEC)); f.write(data('carry', CARRY))
    f.write(data('pow10', [10 ** i for i in range(MAXDIGITS)]))
    f.write(f"{len(stub)} >Stublen\n")
    for nm, v in [('Rels', rels), ('Rell', rell), ('Relp', relp), ('Kn', K), ('Ncode', NCODE), ('Cdata', DATA), ('Cstr', STR), ('Cnone', NONE), ('Cerr', ERR),
                  ('Cscatter', SCATTER), ('Ctee', TEE), ('Cteec', TEEC), ('Cvect', VECT), ('Cbytes', KW['bytes']), ('Cvec', VEC), ('Clen', LEN), ('Nsig5', 5), ('Slot', SLOT), ('Nd', ND), ('Vbase', VBASE), ('Maxname', MAXNAME), ('Maxdig', MAXDIGITS),
                  ('Ha1', HA1), ('Hb1', HB1), ('Hs1', HS1), ('Ha2', HA2), ('Hb2', HB2), ('Hs2', HS2)]\
                 + [('Ru%d' % r, RACE_U[r]) for r in range(ROUNDS)] + [('Rv%d' % r, RACE_V[r]) for r in range(ROUNDS)]:
        f.write(f"{v} >{nm}\n")
print(f"stencils: {len(S)} bodies + {len(PRE)} prologues, {len(sten)} bytes; stub {len(stub)} bytes; tables.strand {os.path.getsize(f'{ROOT}/src/tables.strand')} bytes")
