// Byte anchors for the Linux engine component walk (Steam build 35924).
// These are the engine's OWN PlayerOwned read, inside the function the port
// already patches for `showicons`. They fix the pool layout and the stride
// that ecs_linux.cpp uses. Evidence: docs/re/linux/DEV_D6DB920F.md.
#pragma once
#include <cstdint>
#include <cstddef>

struct EcsCheck { uintptr_t rva; const char* bytes; size_t size; };

static const EcsCheck kEcsChecks[] = {
    // flat: rdx = [pool+0xb8]; rax = entity slot; lea rax,[rdx+rax*4]; mov eax,[rax]
    {0x138babb, "\x48\x8b\x91\xb8\x00\x00\x00\x48\x98\x48\x8d\x04\x82\x8b\x00", 15},
    // paged: sar edx,5; movsxd; shl rdx,4; add rdx,[pool+0xd0]
    {0x138ba52, "\xc1\xfa\x05\x48\x63\xd2\x48\xc1\xe2\x04\x48\x03\x91\xd0\x00\x00\x00", 17},
    // page + (slot & 31) * 4
    {0x138ba66, "\x48\x8d\x04\x82\x8b\x00", 6},
    // the pool table itself: rdx = [engine+0x80]; rcx = [rdx + type*8]
    {0x138baa6, "\x49\x8b\x96\x80\x00\x00\x00\x49\x63\xcd\x48\x8b\x0c\xca", 14},
};
