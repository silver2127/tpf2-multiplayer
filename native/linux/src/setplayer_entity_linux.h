#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// Build 35924: immediately after Lua argument conversion, before component
// dispatch. The marked path constructs the same engine mutation scope as the
// ordinary paths and rejoins the generic setter and common scope destructor.
static constexpr uintptr_t OWNER_DISPATCH = 0x1dc4f8f;
static constexpr uint8_t OWNER_DISPATCH_BYTES[] = {
    0x48,0x8d,0x05,0x02,0xe4,0xc3,0x03, // lea rax, Construction typeinfo
    0x4c,0x8d,0xbd,0x00,0xff,0xff,0xff // lea r15, [rbp-100h]
};

static inline std::vector<uint8_t> EntityOwnerRelay(uintptr_t at, uintptr_t type,
        uintptr_t scope, uintptr_t generic, uintptr_t ordinary)
{
    std::vector<uint8_t> code;
    auto bytes = [&](std::initializer_list<uint8_t> b) { code.insert(code.end(),b); };
    auto branch = [&](uint8_t op, uintptr_t target) {
        code.push_back(op);
        const int32_t rel = static_cast<int32_t>(target - (at + code.size() + 4));
        const auto* p = reinterpret_cast<const uint8_t*>(&rel);
        code.insert(code.end(),p,p+4);
    };
    bytes({0x89,0xf0, 0x25,0,0,0,0xf0, 0x3d,0,0,0,0x60}); // eax = esi & mask; cmp tag
    bytes({0x75,0}); const size_t ordinaryJump = code.size()-1;
    bytes({0x81,0xe6,0xff,0xff,0xff,0x0f}); // strip marker from player
    bytes({0x89,0xb5,0x54,0xfe,0xff,0xff}); // player at rbp-1ac
    bytes({0x48,0x8d,0xbd,0x58,0xfe,0xff,0xff}); // engine scope at rbp-1a8
    bytes({0x48,0x89,0xbd,0x18,0xfe,0xff,0xff}); // common-exit scope pointer
    bytes({0x48,0x89,0xde}); // engine (rbx) -> second scope-ctor arg
    branch(0xe8,scope);
    bytes({0x4c,0x8d,0xad,0x50,0xfe,0xff,0xff}); // generic setter's entity pointer
    branch(0xe9,generic);
    code[ordinaryJump] = static_cast<uint8_t>(code.size() - ordinaryJump - 1);
    bytes({0x48,0xb8}); // relocate the stolen RIP-relative lea
    const auto* p = reinterpret_cast<const uint8_t*>(&type);
    code.insert(code.end(),p,p+8);
    bytes({0x4c,0x8d,0xbd,0x00,0xff,0xff,0xff});
    branch(0xe9,ordinary);
    return code;
}
