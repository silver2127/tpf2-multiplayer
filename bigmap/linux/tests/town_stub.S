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
# int TestOctreeLevel(entry, id): the skip query's register contract
# (esi = id, rdx = live value later moved to r12); returns r14d, or -1 if
# r12 was not the stock `mov r12,rdx`.
.globl TestOctreeLevel
.type TestOctreeLevel,@function
TestOctreeLevel:
 push %r12
 push %r14
 mov $0x1234,%edx
 test %esi,%esi
 jmp *%rdi
.globl TestOctreeLevelReturn
TestOctreeLevelReturn:
 mov %r14d,%eax
 cmp $0x1234,%r12
 je 1f
 mov $-1,%eax
1:
 pop %r14
 pop %r12
 ret
# int TestOctreeChild(entry, parent, children, octant, running, remaining, slot*):
# the descent loop's child-step contract (rax parent, rdx child array,
# r15d octant, r13d running index, r9d remaining, edi created flag).
# Returns r13d and stores rbx to *slot; -2 if rsi/edi broke the contract.
.globl TestOctreeChild
.type TestOctreeChild,@function
TestOctreeChild:
 push %rbx
 push %r12
 push %r13
 push %r15
 mov 40(%rsp),%r12
 push %rsi
 mov %rdi,%r11
 mov %rsi,%rax
 mov %ecx,%r15d
 mov %r8d,%r13d
 mov $0x77,%edi
 jmp *%r11
.globl TestOctreeChildReturn
TestOctreeChildReturn:
 pop %rcx
 mov %rbx,(%r12)
 mov %r13d,%eax
 cmp %rcx,%rsi
 jne 2f
 cmp $0x77,%edi
 je 3f
2:
 mov $-2,%eax
3:
 pop %r15
 pop %r13
 pop %r12
 pop %rbx
 ret
.section .note.GNU-stack,"",@progbits
