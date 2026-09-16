// near_alloc.h -- memory within rel32 reach of the game's code, for pointing a
// `call rel32` at our own function (the Linux counterpart of speedhook.cpp's
// AllocNear). Our libraries load far from the executable, so the call has to
// land on a small stub near it first: mov rax, imm64 ; jmp rax.
// Header-only so each library's build stays single-file.
#pragma once
#include "codewrite_linux.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>

// A page within +/-2 GB of `anchor`, readable and writable, or nullptr.
// MAP_FIXED_NOREPLACE never displaces an existing mapping: an address inside
// the game image (or anything else) just fails and the search moves on.
static inline void* Tpf2mpAllocNear(uintptr_t anchor, size_t len = 4096)
{
    const uintptr_t step = 0x100000;   // 1 MB
    for (uintptr_t d = step; d < 0x70000000; d += step) {
        for (int s = 0; s < 2; s++) {
            if (s == 1 && anchor < d) continue;
            const uintptr_t want = (s ? anchor - d : anchor + d) & ~(uintptr_t)0xfff;
            void* p = mmap((void*)want, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            if ((uintptr_t)p == want) return p;
            munmap(p, len);   // a kernel older than 4.17 took the flag as a hint and placed it elsewhere
        }
    }
    return nullptr;
}

// Point the `call rel32` at `site` at `target` through a stub near the site.
// `expectedCallee` is what the call reaches today; anything else is refused.
// Returns false, changing nothing, when the site is not that call or no stub
// fits within reach.
static inline bool Tpf2mpRedirectCall(uintptr_t site, uintptr_t expectedCallee, void* target)
{
    const uint8_t* s = (const uint8_t*)site;
    int32_t rel;
    memcpy(&rel, s + 1, 4);
    if (s[0] != 0xE8 || site + 5 + (intptr_t)rel != expectedCallee) return false;
    void* stub = Tpf2mpAllocNear(site);
    if (!stub) return false;
    uint8_t code[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };   // mov rax, imm64 ; jmp rax
    const uintptr_t t = (uintptr_t)target;
    memcpy(code + 2, &t, 8);
    memcpy(stub, code, sizeof(code));
    if (mprotect(stub, 4096, PROT_READ | PROT_EXEC) != 0) { munmap(stub, 4096); return false; }
    const intptr_t d = (intptr_t)stub - (intptr_t)(site + 5);
    if (d > INT32_MAX || d < INT32_MIN) { munmap(stub, 4096); return false; }
    const int32_t rel2 = (int32_t)d;
    int err = 0;
    const int result = Tpf2mpCodeWriteSelf(site + 1, (const uint8_t*)&rel2, sizeof(rel2), &err);
    if (result == TPF2MP_CW_UNAVAILABLE) munmap(stub, 4096);
    // Keep the stub mapped after any write: a thread may already have entered.
    return result == TPF2MP_CW_OK;
}
