# Startup / exit stub. Sits at a fixed address right after the ELF headers.
# This is the scalar glue the language cannot express: syscalls. See SCALAR_LEDGER.md.
# Layout contract with the stencils (rbx = address of the first byte after this blob):
#   [rbx-128..-64)  dwords 0..15 (iota constant)
#   [rbx-640..-128) free-list heads, one per power-of-two size class
#   [rbx-648] address of the output flush routine (r8 = end of pending output)
#   [rbx-64] heap limit (exceeding it is a runtime failure)   [rbx-56] output flush threshold
#   [rbx-44] the constant 16 (lane-index increment)
#   [rbx-40] heap base (addresses below it are static data and are never released)
#   [rbx-32] output cursor   [rbx-24] output base   [rbx-16] input ptr   [rbx-8] input length
#   [rbx]    size in bytes of the data image that follows; generated code starts right after it
.intel_syntax noprefix
.text
  .quad 0x1000                      # p_align: the last field of the program header
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
  cmp rax,-4096
  ja nomem
  mov r13,rax
  mov [rbx-40],rax
  movabs rcx,0xffffff000
  add rcx,rax
  mov [rbx-64],rcx
  mov eax,9                         # mmap output buffer: 256 KiB, reused; flushed when full
  xor edi,edi
  mov esi,0x41000
  mov edx,3
  mov r10d,0x22
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
  lea r15,[rip+exit]
  mov eax,[rbx]
  lea rcx,[rbx+rax]
  jmp rcx
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
  .fill 64,1,0
  .fill 512,1,0
  .long 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  .fill 20,1,0
  .long 16
  .fill 40,1,0
dataend:
