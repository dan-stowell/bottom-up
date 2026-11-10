# Startup / exit stub and thread pool. Sits at a fixed address right after the ELF headers.
# This is the scalar glue the language cannot express: syscalls and thread management.
# See SCALAR_LEDGER.md.
#
# Layout contract with the stencils (rbx = address of the first byte after this blob):
#   [rbx-704] out stride of the current line: 0 = no out sink, 1 = out, 4 = out4
#   [rbx-696] fork threshold: lines with at least this many elements may run on all cores
#   [rbx-688] address of the exit routine
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
  mov [rbx-16],r13
  test r12,r12
  jz inputdone
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
  jle rddone
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
  kxnorw k5,k5,k5                   # keep results (cleared only for warm-up chunks)
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
  push rax
  push rcx
  push rdx
  push rsi
  push rdi
  push r11
  mov rsi,[rbx-24]
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
  mov r8,[rbx-24]
  pop r11
  pop rdi
  pop rsi
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
# Runs elements [0, m) on all threads (in blocks, written out in order, if the line has an out
# sink), combines the accumulators, and returns with rax = m so the caller's own loop finishes
# the tail [m, n) serially. Each range starts with a one-chunk warm-up so `prev` carries are right.
fork:
  mov [rip+j_head],r11
  mov [rip+j_rdi],rdi
  mov [rip+j_n],r12
  cmp dword ptr [rip+started],0
  jne 1f
  call startthreads
1:
  mov eax,[rbx-704]
  mov [rip+f_stride],rax
  mov qword ptr [rip+f_bs],0
  test eax,eax
  jz fblock
  call flush                        # pending output goes first; r8 = output base
  mov rax,[rbx-24]                  # no streaming flushes while blocks are in flight
  add rax,0x4000000
  mov [rbx-56],rax
fblock:
  mov rsi,[rip+f_bs]
  mov rcx,r12                       # rcx = end of this block
  mov rax,[rip+f_stride]
  test rax,rax
  jz 3f
  mov edx,0x400000                  # 4 MiB of output per block
  cmp eax,4
  jne 2f
  mov edx,0x100000
2:
  add rdx,rsi
  cmp rdx,rcx
  cmovb rcx,rdx
3:
  mov rax,rcx
  sub rax,rsi
  cmp rcx,r12
  jne 4f
  dec rax                           # always leave at least one element for the serial tail
4:
  shr rax,4
  xor edx,edx
  mov r9d,[rip+nthreads]
  div r9                            # chunks per thread
  test rax,rax
  jz fdone
  shl rax,4
  mov [rip+f_per],rax
  lea r10,[rip+wparams]
  mov rdx,rsi
  mov ecx,r9d
  dec ecx
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
  add r10,192
  dec ecx
  jnz 5b
  mov [rip+f_lo],rdx                # the main thread takes the last range
  lea rdi,[rdx+rax]
  mov [rip+f_hi],rdi
  mov rdi,rdx
  sub rdi,rsi
  imul rdi,[rip+f_stride]
  add rdi,[rbx-24]
  mov [rip+f_r8],rdi
  mov eax,r9d
  dec eax
  mov [rip+done],eax
  lock inc dword ptr [rip+seq]
  mov eax,202                       # futex(&seq, WAKE|PRIVATE, 64)
  lea rdi,[rip+seq]
  mov esi,129
  mov edx,64
  syscall
  mov rdi,[rip+j_rdi]
  mov r11,[rip+j_head]
  kxorw k5,k5,k5                    # warm-up chunk: compute, keep nothing, ignore check failures
  lea r14,[rip+justret]
  mov rax,[rip+f_lo]
  sub rax,16
  vpbroadcastd zmm26,eax
  vpaddd zmm26,zmm26,zmm30
  lea rbp,[rax+16]
  mov r8,[rbx-24]
  lea r15,[rip+6f]
  jmp r11
6:
  kxnorw k5,k5,k5
  lea r14,[rip+fail]
  mov rax,[rip+f_lo]
  vpbroadcastd zmm26,eax
  vpaddd zmm26,zmm26,zmm30
  mov rbp,[rip+f_hi]
  mov r8,[rip+f_r8]
  lea r15,[rip+7f]
  jmp r11
7:
  mov ecx,20000                     # wait for the workers: spin briefly, then sleep
8:
  cmp dword ptr [rip+done],0
  je 9f
  pause
  dec ecx
  jnz 8b
  mov edx,[rip+done]
  test edx,edx
  jz 9f
  mov eax,202                       # futex(&done, WAIT|PRIVATE, edx)
  lea rdi,[rip+done]
  mov esi,128
  xor r10d,r10d
  syscall
  mov ecx,1
  jmp 8b
9:
  mov rdi,[rip+j_rdi]
  mov r11,[rip+j_head]
  lea r10,[rip+wparams]
  mov ecx,[rip+nthreads]
  dec ecx
10:
  vpaddd zmm28,zmm28,[r10+64]
  vpmaxsd zmm29,zmm29,[r10+128]
  add r10,192
  dec ecx
  jnz 10b
  mov r9d,[rip+nthreads]
  mov rax,[rip+f_per]
  imul rax,r9
  mov rsi,[rip+f_bs]
  add rsi,rax
  mov [rip+f_bs],rsi
  mov rdx,[rip+f_stride]
  test rdx,rdx
  jz fblock
  imul rax,rdx
  mov r8,[rbx-24]
  add r8,rax
  call flush                        # this block's output, in order
  jmp fblock
fdone:
  mov rdi,[rip+j_rdi]               # syscalls above clobbered these
  mov r11,[rip+j_head]
  mov rax,[rbx-24]
  add rax,0x40000
  mov [rbx-56],rax
  mov rax,[rip+f_bs]
  vpbroadcastd zmm26,eax
  vpaddd zmm26,zmm26,zmm30
  mov rbp,r12
  xor r15d,r15d
  ret

startthreads:
  push r13
  push r8
  mov dword ptr [rip+started],1
  lea r13,[rip+wparams]
  mov r15d,[rip+nthreads]
  dec r15d
1:
  call newstack
  mov rsi,rax
  mov eax,[rip+seq]
  mov [r13+24],eax
  mov eax,56                        # clone(VM|FS|FILES|SIGHAND|THREAD|SYSVSEM, stack)
  mov edi,0x50f00
  xor edx,edx
  xor r10d,r10d
  xor r8d,r8d
  syscall
  test rax,rax
  jz worker
  js nomem
  add r13,192
  dec r15d
  jnz 1b
  pop r8
  pop r13
  ret

worker:                             # r13 = this worker's parameter block
  vmovdqu32 zmm30,[rbx-128]
  vpbroadcastd zmm31,dword ptr [rbx-124]
  lea r14,[rip+fail]
  kxnorw k5,k5,k5
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
  mov r11,[rip+j_head]
  mov rdi,[rip+j_rdi]
  mov r12,[rip+j_n]
  vpbroadcastd zmm27,r12d
  vpxord zmm16,zmm16,zmm16
  vmovdqa32 zmm17,zmm16
  vmovdqa32 zmm18,zmm16
  vmovdqa32 zmm19,zmm16
  vmovdqa32 zmm20,zmm16
  vmovdqa32 zmm21,zmm16
  vmovdqa32 zmm22,zmm16
  vmovdqa32 zmm23,zmm16
  vpxord zmm28,zmm28,zmm28
  vpslld zmm29,zmm31,31
  mov rax,[r13]
  test rax,rax
  jz 3f
  kxorw k5,k5,k5                    # warm-up chunk: compute, keep nothing, ignore check failures
  lea r14,[rip+justret]
  sub rax,16
  vpbroadcastd zmm26,eax
  vpaddd zmm26,zmm26,zmm30
  lea rbp,[rax+16]
  mov r8,[rbx-24]
  lea r15,[rip+3f]
  jmp r11
3:
  kxnorw k5,k5,k5
  lea r14,[rip+fail]
  mov rax,[r13]
  vpbroadcastd zmm26,eax
  vpaddd zmm26,zmm26,zmm30
  mov rbp,[r13+8]
  mov r8,[r13+16]
  lea r15,[rip+4f]
  jmp r11
4:
  vmovdqu32 [r13+64],zmm28
  vmovdqu32 [r13+128],zmm29
  lock dec dword ptr [rip+done]
  jnz wloop
  mov eax,202                       # futex(&done, WAKE|PRIVATE, 1)
  lea rdi,[rip+done]
  mov esi,129
  mov edx,1
  syscall
  jmp wloop

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
frame:    .quad 0
stksize:  .quad 0
j_head:   .quad 0
j_rdi:    .quad 0
j_n:      .quad 0
f_stride: .quad 0
f_bs:     .quad 0
f_per:    .quad 0
f_lo:     .quad 0
f_hi:     .quad 0
f_r8:     .quad 0
  .balign 64
wparams:  .fill 192*16,1,0          # per worker: lo, hi, out ptr, last seq | sum acc | max acc
  .balign 64
  .fill 64,1,0
  .fill 512,1,0
  .long 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  .fill 20,1,0
  .long 16
  .fill 40,1,0
dataend:
