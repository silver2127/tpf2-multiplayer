// The game uses dynamic libgcc/libstdc++; this plugin hides a static runtime.
// Catch foreign exceptions before they reach a C++ cleanup personality here.
// Adapted from the regression-tested menu_game_linux.cpp guard. Only trivial
// frames (the argument tuple and callback) sit between this guard and the game.
#pragma once
#include <dlfcn.h>
#include <tuple>
#include <type_traits>
extern "C" {
__attribute__((visibility("hidden"))) void* tpf2mp_preview_personality = nullptr;
__attribute__((visibility("hidden"))) void* tpf2mp_preview_begin_catch = nullptr;
__attribute__((visibility("hidden"))) void* tpf2mp_preview_end_catch = nullptr;
__attribute__((visibility("hidden"))) void* tpf2mp_preview_unwind_resume = nullptr;
__attribute__((visibility("hidden"))) int tpf2mp_preview_guarded(void (*fn)(void*), void* ctx);
}
__asm__(R"ASM(
    .text
    .p2align 4
    .type   tpf2mp_preview_guarded, @function
tpf2mp_preview_guarded:
.Lpreview_fb:
    .cfi_startproc
    .cfi_personality 0x9b, tpf2mp_preview_personality
    .cfi_lsda 0x1b, .Lpreview_lsda
    endbr64
    subq    $8, %rsp
    .cfi_def_cfa_offset 16
    movq    %rdi, %rax
    movq    %rsi, %rdi
.Lpreview_call_b:
    call    *%rax
.Lpreview_call_e:
    xorl    %eax, %eax
.Lpreview_ret:
    addq    $8, %rsp
    .cfi_remember_state
    .cfi_def_cfa_offset 8
    ret
.Lpreview_pad:
    .cfi_restore_state
    endbr64
    movq    (%rax), %rdx
    andq    $-256, %rdx
    movabsq $0x474e5543432b2b00, %rcx
    cmpq    %rcx, %rdx
    jne     .Lpreview_foreign
    movq    %rax, %rdi
    call    *tpf2mp_preview_begin_catch(%rip)
    call    *tpf2mp_preview_end_catch(%rip)
    movl    $1, %eax
    jmp     .Lpreview_ret
.Lpreview_foreign:
    movq    %rax, %rdi
    call    *tpf2mp_preview_unwind_resume(%rip)
    ud2
.Lpreview_pad_e:
    .cfi_endproc
    .size   tpf2mp_preview_guarded, .-tpf2mp_preview_guarded

    .section .gcc_except_table,"a",@progbits
    .p2align 2
.Lpreview_lsda:
    .byte   0xff
    .byte   0x9b
    .uleb128 .Lpreview_tt-.Lpreview_ttd
.Lpreview_ttd:
    .byte   0x1
    .uleb128 .Lpreview_cse-.Lpreview_csb
.Lpreview_csb:
    .uleb128 .Lpreview_call_b-.Lpreview_fb
    .uleb128 .Lpreview_call_e-.Lpreview_call_b
    .uleb128 .Lpreview_pad-.Lpreview_fb
    .uleb128 0x1
    .uleb128 .Lpreview_pad-.Lpreview_fb
    .uleb128 .Lpreview_pad_e-.Lpreview_pad
    .uleb128 0
    .uleb128 0
.Lpreview_cse:
    .byte   0x1
    .byte   0
    .p2align 2
    .long   0
.Lpreview_tt:
    .text
)ASM");


namespace {
bool resolveGameRuntime() {
    Dl_info self{};
    if(!dladdr(reinterpret_cast<void*>(resolveGameRuntime),&self)) return false;
    struct Need { const char* name; void** out; void* found; };
    Need need[]={
        {"__gxx_personality_v0",&tpf2mp_preview_personality,nullptr},
        {"__cxa_begin_catch",&tpf2mp_preview_begin_catch,nullptr},
        {"__cxa_end_catch",&tpf2mp_preview_end_catch,nullptr},
        {"_Unwind_Resume",&tpf2mp_preview_unwind_resume,nullptr}
    };
    for(auto& n:need) {
        n.found=dlsym(RTLD_DEFAULT,n.name);
        Dl_info location{};
        if(!n.found || !dladdr(n.found,&location) || location.dli_fbase==self.dli_fbase) return false;
    }
    for(const auto& n:need) *n.out=n.found;
    return true;
}
template<class Fn,class... Args> bool callGame(Fn fn,Args... args) {
    struct Call { Fn function; std::tuple<Args...> arguments; } call{fn,{args...}};
    static_assert(std::is_trivially_destructible<Call>::value,"no foreign-frame cleanups");
    return tpf2mp_preview_guarded([](void* opaque) {
        auto* c=static_cast<Call*>(opaque);
        std::apply(c->function,c->arguments);
    },&call)==0;
}
template<class R,class Fn,class... Args> bool callGameResult(R* result,Fn fn,Args... args) {
    struct Call { R* result; Fn function; std::tuple<Args...> arguments; } call{result,fn,{args...}};
    static_assert(std::is_trivially_destructible<Call>::value,"no foreign-frame cleanups");
    return tpf2mp_preview_guarded([](void* opaque) {
        auto* c=static_cast<Call*>(opaque);
        *c->result=std::apply(c->function,c->arguments);
    },&call)==0;
}
}
