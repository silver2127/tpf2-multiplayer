#include "../src/setplayer_entity_linux.h"
#include "../src/near_alloc.h"
#include <cassert>
#include <cstdio>

extern "C" {
uintptr_t ownerSeenType, ownerSeenTemp, ownerExpectedTemp, ownerSeenScope, ownerExpectedScope;
uintptr_t ownerSeenEngine;
int ownerSeenPlayer, ownerSeenEntity, ownerScopeCalls;
void OwnerScope(void* scope, void* engine) {
    ++ownerScopeCalls; ownerSeenScope=reinterpret_cast<uintptr_t>(scope);
    ownerSeenEngine=reinterpret_cast<uintptr_t>(engine);
}
__attribute__((naked)) void OwnerDone() {
    asm volatile("add $0x228,%rsp; pop %r15; pop %r13; pop %rbx; pop %rbp; ret");
}
__attribute__((naked)) void OwnerGeneric() {
    asm volatile("mov -0x1ac(%rbp),%eax; mov %eax,ownerSeenPlayer(%rip);"
                 "mov (%r13),%eax; mov %eax,ownerSeenEntity(%rip);"
                 "mov -0x1e8(%rbp),%rax; mov %rax,ownerExpectedScope(%rip);"
                 "mov $1,%eax; jmp OwnerDone");
}
__attribute__((naked)) void OwnerOrdinary() {
    asm volatile("mov %rax,ownerSeenType(%rip); mov %r15,ownerSeenTemp(%rip);"
                 "lea -0x100(%rbp),%rax; mov %rax,ownerExpectedTemp(%rip);"
                 "mov %esi,ownerSeenPlayer(%rip); xor %eax,%eax; jmp OwnerDone");
}
__attribute__((naked)) int OwnerInvoke(void*, int, int, void*) {
    asm volatile("push %rbp; mov %rsp,%rbp; push %rbx; push %r13; push %r15; sub $0x228,%rsp;"
                 "mov %rcx,%rbx; mov %edx,-0x1b0(%rbp); jmp *%rdi");
}
}

int main() {
    void* memory=Tpf2mpAllocNear(reinterpret_cast<uintptr_t>(&OwnerInvoke)); assert(memory);
    auto code=EntityOwnerRelay(reinterpret_cast<uintptr_t>(memory),0xdeadbeef,
        reinterpret_cast<uintptr_t>(&OwnerScope),reinterpret_cast<uintptr_t>(&OwnerGeneric),
        reinterpret_cast<uintptr_t>(&OwnerOrdinary));
    memcpy(memory,code.data(),code.size()); assert(!mprotect(memory,4096,PROT_READ|PROT_EXEC));
    for(int player : {0,42,-1,0x50000001,0x70000001}) {
        assert(OwnerInvoke(memory,player,719,reinterpret_cast<void*>(0x1234))==0);
        assert(ownerSeenPlayer==player && ownerSeenType==0xdeadbeef);
        assert(ownerSeenTemp==ownerExpectedTemp && ownerScopeCalls==0);
    }
    for(int player : {0,42,0x0fffffff}) {
        const int before=ownerScopeCalls;
        assert(OwnerInvoke(memory,0x60000000|player,719,reinterpret_cast<void*>(0x1234))==1);
        assert(ownerSeenPlayer==player && ownerSeenEntity==719);
        assert(ownerScopeCalls==before+1 && ownerSeenScope==ownerExpectedScope && ownerSeenEngine==0x1234);
    }
    munmap(memory,4096);
    puts("PASS: executable entity-owner relay: ordinary dispatch, marker decoding, engine scope and generic setter ABI");
}
