.text
.globl TestTown
.type TestTown,@function
TestTown:
 push %rbp
 sub $512,%rsp
 lea 512(%rsp),%rbp
 mov %esi,%eax
 call *%rdi
 movss -444(%rbp),%xmm0
 add $512,%rsp
 pop %rbp
 ret
.globl TestMinMaxBridge
.type TestMinMaxBridge,@function
TestMinMaxBridge:
 push %rbp
 push %rbx
 push %r14
 mov %rdi,%rbx
 mov %rsi,%r14
 lea .Lminmax_done(%rip),%rax
 mov %rax,bigmap_minmax_return(%rip)
 jmp TerrainMinMaxBridge
.Lminmax_done:
 shl $16,%edx
 or %edx,%eax
 pop %r14
 pop %rbx
 pop %rbp
 ret
.section .note.GNU-stack,"",@progbits
