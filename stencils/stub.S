# Startup / exit stub and thread pool. Sits at a fixed address right after the ELF headers.
# This is the scalar glue the language cannot express: syscalls and thread management.
# See SCALAR_LEDGER.md.
#
# Layout contract with the stencils (rbx = address of the first byte after this blob):
#   [rbx-960..-768) constants of the automaton stencils (see the end of this file)
#   [rbx-712] carries the current line uses: bit r = running sum in zmm16+r, bit 8+r = running max
#             in zmm8+r, bit 16+r = prev in zmm16+r, bit 24+r = automaton function in zmm16+r
#   [rbx-708] chunk size of the current line: 16 (dword lanes) or 64 (byte lanes)
#   [rbx-704] out stride of the current line: 0 = no out sink, 1 = out, 4 = out4
#   [rbx-696] fork threshold: lines with at least this many elements may run on all cores
#   [rbx-688] address of the exit routine
#   [rbx-680] id of the automaton token that is the last carry user of the current line, else 0
#   [rbx-676] the same id while the pool runs a dry pass, else 0: that token ends the loop body early
#   [rbx-672] the current line's vote: 1 = may be split across threads, 0 = some token vetoed
#   [rbx-656] address of the fork routine      [rbx-648] address of the output flush routine
#   [rbx-640..-128) free-list heads, one per power-of-two size class
#   [rbx-128..-64)  dwords 0..15 (iota constant)
#   [rbx-64] heap limit (exceeding it is a runtime failure)   [rbx-56] output flush threshold
#   [rbx-44] the constant 16 (lane-index increment)
#   [rbx-40] heap base (addresses below it are static data and are never released)
#   [rbx-32] output cursor   [rbx-24] output base   [rbx-16] input ptr   [rbx-8] input length
#   [rbx]    size in bytes of the data image that follows; generated code starts right after it
#   [rbx+8]  bytes of chunk-local storage each thread needs above its stack pointer
# Register contract: r11 loop head, r12 n, rax index, rbp end of this thread's range,
#   r15 = 0 on the main path, else where a pool thread continues when its range is done,
#   r14 fail handler, r13 bump pointer (main) / parameter block (workers),
#   zmm26 lane indices, zmm27 n, zmm28/29 sum/max accumulators, zmm30 iota, zmm31 ones.
.intel_syntax noprefix
.text
  .quad 0x1000                      # p_align: the last field of the program header
_start:
  lea rbx,[rip+dataend]
  mov r15,[rsp]                     # argc: any argument = "eager" mode, fork even tiny lines (tests)
  # how many CPUs may we use?
  sub rsp,128
  mov qword ptr [rsp],0
  mov qword ptr [rsp+8],0
  mov eax,204                       # sched_getaffinity(0, 128, rsp)
  xor edi,edi
  mov esi,128
  mov rdx,rsp
  syscall
  popcnt rcx,[rsp]
  popcnt rdx,[rsp+8]
  add ecx,edx
  mov edx,16
  cmp ecx,edx
  cmova ecx,edx
  test ecx,ecx
  jnz 1f
  mov ecx,1
1:
  mov [rip+nthreads],ecx
  mov rax,0x7fffffffffffffff
  cmp ecx,1
  je 2f
  mov eax,0x80000                   # default: split lines of 512 K elements or more
  cmp r15,1
  jbe 2f
  mov eax,64
  mov qword ptr [rip+f_blk],4096
  mov qword ptr [rip+f_blkscan],4096
  mov qword ptr [rip+f_blkprobe],1024
  mov qword ptr [rip+f_blkout],8192
2:
  mov [rbx-696],rax
  add rsp,128
  # every thread, this one included, runs on its own stack with room for chunk-local vectors
  mov eax,[rbx+8]
  mov [rip+frame],rax
  add rax,0x40000
  mov [rip+stksize],rax
  call newstack
  mov rsp,rax
  mov eax,9                         # mmap heap: 64 GiB, lazily committed
  xor edi,edi
  movabs rsi,0x1000000000
  mov edx,3
  mov r10d,0x4022                   # PRIVATE|ANONYMOUS|NORESERVE
  mov r8,-1
  xor r9d,r9d
  syscall
  cmp rax,-4096
  ja nomem
  mov r13,rax
  mov [rbx-40],rax
  movabs rcx,0xffffff000
  add rcx,rax
  mov [rbx-64],rcx
  mov eax,9                         # mmap output buffer: 64 MiB reserved; streamed in 256 KiB pieces
  xor edi,edi
  mov esi,0x4001000
  mov edx,3
  mov r10d,0x4022
  mov r8,-1
  xor r9d,r9d
  syscall
  cmp rax,-4096
  ja nomem
  mov [rbx-24],rax
  mov [rbx-32],rax
  add rax,0x40000
  mov [rbx-56],rax
  lea rax,[rip+flush]
  mov [rbx-648],rax
  lea rax,[rip+fork]
  mov [rbx-656],rax
  lea rax,[rip+exit]
  mov [rbx-688],rax
  # input: if stdin is a regular file at offset 0, map it (no copy, no fresh pages); else read it all
  sub rsp,160
  mov eax,5                         # fstat(0, rsp)
  xor edi,edi
  mov rsi,rsp
  syscall
  test eax,eax
  jnz useread
  mov eax,[rsp+24]                  # st_mode
  and eax,0xf000
  cmp eax,0x8000                    # S_IFREG
  jne useread
  mov r12,[rsp+48]                  # st_size
  mov eax,8                         # lseek(0, 0, SEEK_CUR) must be 0
  xor edi,edi
  xor esi,esi
  mov edx,1
  syscall
  test rax,rax
  jnz useread
  test r12,r12
  jz useread                        # size 0 may be procfs: read it
  mov [rbx-16],r13
  cmp r12,0x7fe00000                # vectors are limited to 2^31-1 elements
  jae nomem
  mov eax,9                         # mmap(0, size, PROT_READ, MAP_PRIVATE, 0, 0)
  xor edi,edi
  mov rsi,r12
  mov edx,1
  mov r10d,2
  xor r8d,r8d
  xor r9d,r9d
  syscall
  cmp rax,-4096
  ja useread
  mov [rbx-16],rax
  jmp inputdone
useread:
  xor r12d,r12d
rd:
  xor eax,eax                       # read(0, heap+total, 1 MiB)
  xor edi,edi
  lea rsi,[r13+r12]
  mov edx,0x100000
  syscall
  test rax,rax
  jz rddone
  js nomem                          # a read error is a failure, not end of input
  add r12,rax
  cmp r12,0x7fe00000
  jb rd
nomem:
  mov edi,98
  mov eax,231
  syscall
rddone:
  mov [rbx-16],r13
  lea r13,[r13+r12+127]
  and r13,-64
inputdone:
  add rsp,160
  mov [rbx-8],r12
  vmovdqu32 zmm30,[rbx-128]
  vpbroadcastd zmm31,dword ptr [rbx-124]
  lea r14,[rip+fail]
  kxnorq k5,k5,k5                   # keep results (cleared only for warm-up chunks)
  xor r15d,r15d
  mov eax,[rbx]
  lea rcx,[rbx+rax]
  jmp rcx

newstack:                           # rax = initial stack pointer of a fresh stack (frame above it)
  mov eax,9
  xor edi,edi
  mov rsi,[rip+stksize]
  mov edx,3
  mov r10d,0x4022
  mov r8,-1
  xor r9d,r9d
  syscall
  cmp rax,-4096
  ja nomem
  add rax,[rip+stksize]
  sub rax,128
  sub rax,[rip+frame]
  and rax,-64
  ret

flush:                              # write [output base, r8) to stdout; r8 = output base. Preserves everything else.
  push rsi
  mov rsi,[rbx-24]
  call wrange
  mov r8,[rbx-24]
  pop rsi
  ret
wrange:                             # write [rsi, r8) to stdout. Preserves everything but rsi.
  push rax
  push rcx
  push rdx
  push rdi
  push r11
fl1:
  mov rdx,r8
  sub rdx,rsi
  jle fl2
  mov eax,1
  mov edi,1
  syscall
  test rax,rax
  jle wrfail
  add rsi,rax
  jmp fl1
fl2:
  pop r11
  pop rdi
  pop rdx
  pop rcx
  pop rax
  ret
wrfail:
  mov edi,97
  mov eax,231
  syscall
exit:
  mov r8,[rbx-32]
  call flush
  xor edi,edi
  mov eax,231
  syscall

# ------------------------------------------------------------------ thread pool
# fork: called at a line's loop head when the line voted "splittable" and n >= threshold.
# Runs elements [0, m) on all threads, block by block, and returns with rax = m so the caller's
# own loop finishes [m, n) serially.
#
# Every thread (the caller included, as the last one) runs `dojob` on its range:
#   mode 0  plain line: a one-chunk warm-up (computes, stores nothing) so `prev` carries are
#           right, then the range for real.
#   mode 1  dry pass for lines with running sums/maxima: warm-up, then the whole range with
#           nothing stored; reports its carries before (W) and after (OUT).
#   mode 2  real pass starting from the carries the pool computed (IN); reports OUT.
# For scan lines the pool derives each thread's IN from the dry pass, runs the real pass, and then
# *verifies* that every thread's IN equals its predecessor's real OUT. If so the block is exactly
# what a serial run produces. If not (scans feeding scans, or a deferred bounds-check failure),
# the caller's registers are restored and the rest of the line runs serially.
#
# A carry that depends on another carry's *value* (a `prev` of an automaton's state, or of a running
# sum) is wrong in the dry pass. So the dry pass also reports its carries one chunk before the end of
# the range (OUT'), the pool derives every thread's carries one chunk before its range (INw) the same
# way it derives IN, and the real pass starts with a warm-up chunk from INw: after it the `prev`
# carries are exact, they are recorded as that thread's IN, and the verification covers them.
#
# Per-thread block (8192 bytes, allocated when the pool starts): +0 lo, +8 hi, +16 output start,
# +24 last seq, +32 output end, +40 continuation, +48 reset-accumulators flag, +64 scratch,
# +128 sum acc, +192 max acc, +256 W, +1280 IN, +2304 OUT, +3328 OUT', +4352 INw
# (16 zmm each: zmm8..zmm23).
.macro savecar base
  .irp i,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23
  vmovdqu32 [\base+(\i-8)*64],zmm\i
  .endr
.endm
.macro loadcar base
  .irp i,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23
  vmovdqu32 zmm\i,[\base+(\i-8)*64]
  .endr
.endm
.macro setrange                     # rax = first element: lane indices follow
  vpbroadcastd zmm26,eax
  vpaddd zmm26,zmm26,zmm30
.endm

dojob:                              # r13 = this thread's block; continues at [r13+40]
  mov r11,[rip+j_head]
  mov rdi,[rip+j_rdi]
  mov r12,[rip+j_n]
  vpbroadcastd zmm27,r12d
  cmp dword ptr [rip+j_mode],2
  je dj_real
  vpxord zmm16,zmm16,zmm16          # identity carries
  .irp i,17,18,19,20,21,22,23
  vmovdqa32 zmm\i,zmm16
  .endr
  vpslld zmm8,zmm31,31
  .irp i,9,10,11,12,13,14,15
  vmovdqa32 zmm\i,zmm8
  .endr
  mov rax,[r13]
  test rax,rax
  jz dj_warm
  kxorq k5,k5,k5                    # warm-up chunk: compute, keep nothing, ignore check failures
  lea r14,[rip+justret]
  mov rbp,rax
  mov ecx,[rbx-708]
  sub rax,rcx
  setrange
  lea r8,[r13+64]
  lea r15,[rip+dj_warm]
  jmp r11
dj_warm:
  cmp dword ptr [rip+j_mode],1
  jne dj_run
  mov eax,[rbx-712]                 # an automaton's dry pass must report the function of its range
  .irp i,16,17,18,19,20,21,22,23    # alone, so forget what the warm-up chunk did to it
  bt eax,\i+8
  jnc 1f
  vpxord zmm\i,zmm\i,zmm\i
1:
  .endr
  savecar r13+256
  kxorq k5,k5,k5
  lea r14,[rip+justret]
  mov rax,[r13]
  setrange
  mov r8,[r13+16]                   # a fully masked store must still point at mapped memory,
                                    # or every one of them takes a microcode assist
  mov rbp,[r13+8]
  mov ecx,[rbx-708]
  sub rbp,rcx                       # first everything but the last chunk ...
  cmp rbp,rax
  jbe dj_dry1                       # (the range is a single chunk)
  lea r15,[rip+dj_dry1]
  jmp r11
dj_dry1:
  savecar r13+3328                  # ... OUT' ...
  mov rbp,[r13+8]
  lea r15,[rip+dj_dry]
  jmp r11                           # ... then the last chunk
dj_dry:
  savecar r13+2304
  jmp dj_end
dj_real:
  cmp r13,[rip+wbase]
  je dj_real0                       # the first range starts from the caller's true carries
  loadcar r13+4352                  # the others: one warm-up chunk from INw
  kxorq k5,k5,k5
  lea r14,[rip+justret]
  mov rax,[r13]
  mov rbp,rax
  mov ecx,[rbx-708]
  sub rax,rcx
  setrange
  lea r8,[r13+64]
  lea r15,[rip+dj_real1]
  jmp r11
dj_real1:
  mov eax,[rbx-712]                 # the `prev` carries it produced are this thread's IN
  .irp i,16,17,18,19,20,21,22,23
  bt eax,\i
  jnc 1f
  vmovdqu32 [r13+1280+(\i-8)*64],zmm\i
1:
  .endr
dj_real0:
  loadcar r13+1280
dj_run:
  kxnorq k5,k5,k5
  lea r14,[rip+fail]
  lea rax,[rip+softfail]
  cmp dword ptr [rip+j_mode],2
  cmove r14,rax
  cmp dword ptr [r13+48],0
  je 1f
  vpxord zmm28,zmm28,zmm28
  vpslld zmm29,zmm31,31
1:
  mov rax,[r13]
  setrange
  mov rbp,[r13+8]
  mov r8,[r13+16]
  lea r15,[rip+dj_ran]
  jmp r11
dj_ran:
  mov [r13+32],r8
  vmovdqu32 [r13+128],zmm28
  vmovdqu32 [r13+192],zmm29
  savecar r13+2304
dj_end:
  kxnorq k5,k5,k5
  lea r14,[rip+fail]
  jmp [r13+40]

runall:                             # run the current job on every thread; continues at [rip+f_cont]
  mov eax,[rip+nthreads]
  dec eax
  mov [rip+done],eax
  lock inc dword ptr [rip+seq]
  mov eax,202                       # futex(&seq, WAKE|PRIVATE, 64)
  lea rdi,[rip+seq]
  mov esi,129
  mov edx,64
  syscall
  lea rax,[rip+ra1]
  mov [r13+40],rax
  jmp dojob
ra1:
  mov ecx,20000                     # wait for the workers: spin briefly, then sleep
ra2:
  cmp dword ptr [rip+done],0
  je ra3
  pause
  dec ecx
  jnz ra2
  mov edx,[rip+done]
  test edx,edx
  jz ra3
  mov eax,202                       # futex(&done, WAIT|PRIVATE, edx)
  lea rdi,[rip+done]
  mov esi,128
  xor r10d,r10d
  syscall
  mov ecx,1
  jmp ra2
ra3:
  mov rdi,[rip+j_rdi]
  mov r11,[rip+j_head]
  jmp [rip+f_cont]

fork:
  pop qword ptr [rip+f_ret]         # keep rsp exactly as in the serial loop: chunk-local vectors
                                    # live at 64-byte-aligned offsets from it
  mov [rip+j_head],r11
  mov [rip+j_rdi],rdi
  mov [rip+j_n],r12
  mov [rip+f_r13],r13
  mov [rip+f_r8in],r8
  mov qword ptr [rbx-56],-1         # no streaming flushes while the pool owns the line
  cmp dword ptr [rip+started],0
  jne 1f
  call startthreads
1:
  mov eax,[rip+nthreads]            # the caller is the last thread
  dec eax
  shl rax,13
  add rax,[rip+wbase]
  mov r13,rax
  mov dword ptr [r13+48],0
  mov eax,[rbx-704]
  mov [rip+f_stride],rax
  mov qword ptr [rip+f_bs],0
  mov dword ptr [rip+f_probe],1
  test eax,eax
  jz fblock
  call flush                        # pending output goes first; r8 = output base
fblock:
  mov rsi,[rip+f_bs]
  mov rcx,r12                       # rcx = end of this block
  mov rdx,[rip+f_blk]               # block length: everything, unless ...
  mov eax,[rbx-712]
  test eax,0xff00ffff
  jz 2f
  mov rdx,[rip+f_blkscan]           # ... the line has scans (first a small probe block) ...
  cmp dword ptr [rip+f_probe],0
  je 2f
  mov rdx,[rip+f_blkprobe]
2:
  mov rax,[rip+f_stride]
  test rax,rax
  jz 3f
  mov r9,[rip+f_blkout]             # ... or an out sink (output is produced block by block)
  cmp eax,4
  jne 21f
  shr r9,2
21:
  cmp r9,rdx
  cmovb rdx,r9
3:
  add rdx,rsi
  jc 31f
  cmp rdx,rcx
  cmovb rcx,rdx
31:
  mov rax,rcx
  sub rax,rsi
  cmp rcx,r12
  jne 4f
  dec rax                           # always leave at least one element for the serial tail
4:
  shr rax,6
  xor edx,edx
  mov r9d,[rip+nthreads]
  div r9                            # 64-element units per thread
  test rax,rax
  jz fdone
  shl rax,6
  mov [rip+f_per],rax
  mov r10,[rip+wbase]
  mov rdx,rsi
  mov ecx,r9d
5:
  mov [r10],rdx                     # lo
  lea rdi,[rdx+rax]
  mov [r10+8],rdi                   # hi
  mov rdi,rdx
  sub rdi,rsi
  imul rdi,[rip+f_stride]
  add rdi,[rbx-24]
  mov [r10+16],rdi                  # where this range's output goes
  add rdx,rax
  add r10,8192
  dec ecx
  jnz 5b
  mov eax,[rbx-712]
  test eax,0xff00ffff
  jnz fscan
  mov dword ptr [rip+j_mode],0      # plain line: one pass
  lea rax,[rip+fgood]
  mov [rip+f_cont],rax
  jmp runall

fscan:                              # scan line: dry pass, derive carries, real pass, verify
  mov r10,[rip+wbase]
  savecar r10+1280                  # thread 0 starts from the caller's (true) carries
  vmovdqu32 [rip+f_a28],zmm28
  vmovdqu32 [rip+f_a29],zmm29
  mov dword ptr [rip+j_mode],1
  mov eax,[rbx-680]
  mov [rbx-676],eax
  lea rax,[rip+fs1]
  mov [rip+f_cont],rax
  jmp runall
fs1:
  mov dword ptr [rbx-676],0
  mov r10,[rip+wbase]               # IN[t] from IN[t-1], W[t-1], OUT[t-1], by kind of carry
  mov ecx,1
  mov eax,[rbx-712]
fs2:
  xor edx,edx                       # j: 0..7 = zmm8..15 (max carries), 8..15 = zmm16..23 (sum / prev)
fs3:
  mov rsi,rdx
  shl rsi,6
  lea rdi,[r10+rsi]
  vmovdqu32 zmm25,[rdi+2304]        # from OUT: the carries at the start of the next range
  call derive
  vmovdqu32 [rdi+8192+1280],zmm24
  vmovdqu32 zmm25,[rdi+3328]        # from OUT': the carries one chunk before it
  call derive
  vmovdqu32 [rdi+8192+4352],zmm24
  inc edx
  cmp edx,16
  jb fs3
  add r10,8192
  inc ecx
  cmp ecx,[rip+nthreads]
  jb fs2
  mov dword ptr [rip+f_soft],0
  mov dword ptr [rip+j_mode],2
  lea rax,[rip+fs7]
  mov [rip+f_cont],rax
  jmp runall
fs7:
  cmp dword ptr [rip+f_soft],0
  jne fsbad
  mov r10,[rip+wbase]               # verify: IN[t] == real OUT[t-1] for every carry in use
  mov ecx,1
  mov eax,[rbx-712]
  mov r9d,eax
  shr r9d,16
  and r9d,0xff
  or eax,r9d                        # bits 0..7: zmm16+r in use; bits 8..15: zmm8+r in use
  mov r9d,eax
  shr r9d,24
  or eax,r9d
fs8:
  xor edx,edx
fs9:
  lea r9d,[rdx+8]
  and r9d,15
  bt eax,r9d
  jnc fs10
  mov rsi,rdx
  shl rsi,6
  vmovdqu32 zmm24,[r10+rsi+8192+1280]
  vpcmpd k1,zmm24,[r10+rsi+2304],4
  kortestw k1,k1
  jnz fsbad
fs10:
  inc edx
  cmp edx,16
  jb fs9
  add r10,8192
  inc ecx
  cmp ecx,[rip+nthreads]
  jb fs8
  mov dword ptr [rip+f_probe],0
  jmp fgood
derive:                             # zmm24 = carry edx after a range, from IN [rdi+1280], W [rdi+256]
  vmovdqu32 zmm24,[rdi+1280]        # and the range's dry result zmm25; eax = the line's carry kinds
  cmp edx,8
  jae 4f
  lea r9d,[rdx+8]
  bt eax,r9d
  jnc 6f
  vpmaxsd zmm24,zmm24,zmm25         # running max
  ret
4:
  lea r9d,[rdx-8]
  bt eax,r9d
  jnc 5f
  vpaddd zmm24,zmm24,zmm25          # running sum: add what the range added
  vpsubd zmm24,zmm24,[rdi+256]
  ret
5:
  lea r9d,[rdx+8]
  bt eax,r9d
  jnc 55f
  vmovdqa32 zmm24,zmm25             # prev
  ret
55:
  lea r9d,[rdx+16]
  bt eax,r9d
  jnc 6f                            # automaton: OUT o IN (both stored xor identity)
  vpxord zmm24,zmm24,dword ptr [rbx-896]{1to16}
  vpxord zmm25,zmm25,dword ptr [rbx-896]{1to16}
  vpshufb zmm24,zmm25,zmm24
  vpxord zmm24,zmm24,dword ptr [rbx-896]{1to16}
6:
  ret
fsbad:                              # not what a serial run would do: undo and finish serially
  mov r10,[rip+wbase]
  loadcar r10+1280
  vmovdqu32 zmm28,[rip+f_a28]
  vmovdqu32 zmm29,[rip+f_a29]
  jmp fdone

fgood:                              # the block stands: fold in the workers' accumulators, emit output
  mov r10,[rip+wbase]
  mov ecx,[rip+nthreads]
  dec ecx
1:
  vpaddd zmm28,zmm28,[r10+128]
  vpmaxsd zmm29,zmm29,[r10+192]
  add r10,8192
  dec ecx
  jnz 1b
  mov r9d,[rip+nthreads]
  mov rax,[rip+f_per]
  imul rax,r9
  add [rip+f_bs],rax
  cmp qword ptr [rip+f_stride],0
  je fblock
  mov r10,[rip+wbase]               # every thread's region, in order
  mov ecx,[rip+nthreads]
2:
  mov rsi,[r10+16]
  mov r8,[r10+32]
  call wrange
  add r10,8192
  dec ecx
  jnz 2b
  jmp fblock
fdone:
  mov rdi,[rip+j_rdi]               # syscalls above clobbered these
  mov r11,[rip+j_head]
  mov r13,[rip+f_r13]
  mov rax,[rbx-24]
  mov r8,rax
  add rax,0x40000
  mov [rbx-56],rax
  cmp qword ptr [rip+f_stride],0
  jne 1f
  mov r8,[rip+f_r8in]
1:
  mov rax,[rip+f_bs]
  setrange
  mov rbp,r12
  xor r15d,r15d
  lea r14,[rip+fail]
  jmp [rip+f_ret]

startthreads:
  push r13
  push r8
  mov dword ptr [rip+started],1
  mov eax,9                         # per-thread blocks: 17 x 8192
  xor edi,edi
  mov esi,0x22000
  mov edx,3
  mov r10d,0x22
  mov r8,-1
  xor r9d,r9d
  syscall
  cmp rax,-4096
  ja nomem
  mov [rip+wbase],rax
  mov r13,rax
  mov r15d,[rip+nthreads]
  dec r15d
1:
  call newstack
  mov rsi,rax
  mov eax,[rip+seq]
  mov [r13+24],eax
  mov dword ptr [r13+48],1
  lea rax,[rip+wdone]
  mov [r13+40],rax
  mov eax,56                        # clone(VM|FS|FILES|SIGHAND|THREAD|SYSVSEM, stack)
  mov edi,0x50f00
  xor edx,edx
  xor r10d,r10d
  xor r8d,r8d
  syscall
  test rax,rax
  jz worker
  js nomem
  add r13,8192
  dec r15d
  jnz 1b
  pop r8
  pop r13
  ret

worker:                             # r13 = this worker's block
  vmovdqu32 zmm30,[rbx-128]
  vpbroadcastd zmm31,dword ptr [rbx-124]
wloop:
  mov ecx,20000
1:
  mov eax,[rip+seq]
  cmp eax,[r13+24]
  jne 2f
  pause
  dec ecx
  jnz 1b
  mov eax,202                       # futex(&seq, WAIT|PRIVATE, last seen)
  lea rdi,[rip+seq]
  mov esi,128
  mov edx,[r13+24]
  xor r10d,r10d
  syscall
  jmp wloop
2:
  mov [r13+24],eax
  jmp dojob
wdone:
  lock dec dword ptr [rip+done]
  jnz wloop
  mov eax,202                       # futex(&done, WAKE|PRIVATE, 1)
  lea rdi,[rip+done]
  mov esi,129
  mov edx,1
  syscall
  jmp wloop

softfail:                           # a check failed during a speculative pass: remember, carry on
  mov dword ptr [rip+f_soft],1
  ret
justret:
  ret
fail:                               # a runtime check failed; return address identifies the stencil
  pop r12
  lea rsi,[rip+msg]
  mov ecx,15
hex:
  mov rax,r12
  and eax,15
  lea edx,[rax+48]
  lea edi,[rax+87]
  cmp eax,10
  cmovae edx,edi
  mov [rsi+rcx+msghex-msg],dl
  shr r12,4
  dec ecx
  jns hex
  mov eax,1
  mov edi,2
  mov edx,msgend-msg
  syscall
  mov edi,99
  mov eax,231
  syscall
msg:
  .ascii "strand: runtime check failed at 0x"
msghex:
  .ascii "0000000000000000\n"
msgend:
  .balign 64
nthreads: .long 1
started:  .long 0
seq:      .long 0
done:     .long 0
j_mode:   .long 0
f_soft:   .long 0
f_probe:  .long 0
          .long 0
frame:    .quad 0
stksize:  .quad 0
wbase:    .quad 0
j_head:   .quad 0
j_rdi:    .quad 0
j_n:      .quad 0
f_stride: .quad 0
f_bs:     .quad 0
f_per:    .quad 0
f_ret:    .quad 0
f_cont:   .quad 0
f_r13:    .quad 0
f_r8in:   .quad 0
f_blk:      .quad 0x4000000000000000   # elements per block: plain lines (everything)
f_blkscan:  .quad 0x400000             #   lines with scans
f_blkprobe: .quad 0x10000              #   their first, probing, block
f_blkout:   .quad 0x400000             #   bytes of output per block
  .balign 64
f_a28:    .fill 64,1,0
f_a29:    .fill 64,1,0
  .balign 64
  .long 0,0x04040404,0x08080808,0x0c0c0c0c, 0,0x04040404,0x08080808,0x0c0c0c0c   # [rbx-960] dword offsets
  .long 0,0x04040404,0x08080808,0x0c0c0c0c, 0,0x04040404,0x08080808,0x0c0c0c0c   #   within a 128-bit lane
  .long 0x03020100,0x07060504,0x0b0a0908,0x0f0e0d0c, 0x03020100,0x07060504,0x0b0a0908,0x0f0e0d0c   # [rbx-896]
  .long 0x03020100,0x07060504,0x0b0a0908,0x0f0e0d0c, 0x03020100,0x07060504,0x0b0a0908,0x0f0e0d0c   #   identity | offsets
  .long 0x04040404,0x08080808,0x03030303,0x0f0e0d0c                             # [rbx-832] -828 -824 -820
  .quad 0x1111111111111111,0x2222222222222222,0x4444444444444444,0x8888888888888888   # [rbx-816..-784)
  .fill 16,1,0
  .fill 128,1,0                     # runtime variables [rbx-768 .. rbx-640)
  .fill 512,1,0
  .long 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  .fill 20,1,0
  .long 16
  .fill 40,1,0
dataend:
