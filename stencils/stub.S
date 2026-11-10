# Startup / exit stub. Sits at a fixed address right after the ELF headers.
# This is the scalar glue the language cannot express: syscalls. See SCALAR_LEDGER.md.
# Layout contract with the stencils (rbx = address of the first byte after this blob):
#   [rbx-128..-64)  dwords 0..15 (iota constant)
#   [rbx-32] output cursor   [rbx-24] output base   [rbx-16] input ptr   [rbx-8] input length
#   [rbx]    size in bytes of the data image that follows; generated code starts right after it
.intel_syntax noprefix
.text
  .quad 0x1000                      # p_align: the last field of the program header
.globl _start
_start:
  lea rbx,[rip+dataend]
  mov eax,9                         # mmap heap: 64 GiB, lazily committed
  xor edi,edi
  movabs rsi,0x1000000000
  mov edx,3
  mov r10d,0x4022                   # PRIVATE|ANONYMOUS|NORESERVE
  mov r8,-1
  xor r9d,r9d
  syscall
  mov r13,rax
  mov eax,9                         # mmap output buffer: 4 GiB
  xor edi,edi
  movabs rsi,0x100000000
  mov edx,3
  mov r10d,0x4022
  mov r8,-1
  xor r9d,r9d
  syscall
  mov [rbx-24],rax
  mov [rbx-32],rax
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
  jmp rd
rddone:
  mov [rbx-16],r13
  mov [rbx-8],r12
  lea r13,[r13+r12+127]
  and r13,-64
  vmovdqu32 zmm30,[rbx-128]
  vpbroadcastd zmm31,dword ptr [rbx-124]
  lea r14,[rip+fail]
  lea r15,[rip+exit]
  mov eax,[rbx]
  lea rcx,[rbx+rax]
  jmp rcx
exit:
  mov rsi,[rbx-24]
  mov r12,[rbx-32]
wr:
  mov rdx,r12
  sub rdx,rsi
  jle wrdone
  mov eax,1
  mov edi,1
  syscall
  test rax,rax
  jle wrdone
  add rsi,rax
  jmp wr
wrdone:
  xor edi,edi
  mov eax,231
  syscall
.globl fail
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
  .long 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  .fill 64,1,0
dataend:
