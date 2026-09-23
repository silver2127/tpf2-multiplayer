// Byte anchors for the Linux HUD company tint (Steam build 35924).
// Evidence: docs/re/linux/DEV_D6DB920F.md (the live section).
// Every address here was read out of the shipped ELF and, where a register
// contract mattered, watched under gdb in the running game.
#pragma once
#include <cstdint>
#include <cstddef>
#include "ecs_checks_linux.h"   // the engine-walk anchors the tint also needs

struct TintCheck { uintptr_t rva; const char* bytes; size_t size; };

// The two places the icon paths hand (engine, &entity, type) to the engine.
// Both are `call rel32`; Tpf2mpRedirectCall re-checks the target itself, so
// these anchor the *surrounding* instructions that give the registers meaning.
static const TintCheck kTintContextChecks[] = {
    // StationItem::StationItem: r9d = entity -> [rbp-0xc4], rsi = EnginePtr ->
    // [rbp-0xa8]; 0x146f0a0(&EnginePtr) -> engine; then (engine, &entity, type).
    {0x1090299, "\x44\x89\x8d\x3c\xff\xff\xff", 7},                       // mov [rbp-0xc4], r9d
    {0x10902c5, "\x4c\x89\xe7\xe8\xd3\xed\x3d\x00", 8},                   // mov rdi,r12 ; call 146f0a0
    {0x10902cd, "\x48\x8d\x8d\x3c\xff\xff\xff\x8b\x55\x18\x48\x89\xc7\x48\x89\xce", 16},
    // HudIconManager::DoStep, the inlined depot item: the entity is copied to
    // [rbp-0xe9c] and its address becomes rsi for the component lookup.
    {0x1095505, "\x8b\x85\x60\xf1\xff\xff\x89\x85\x64\xf1\xff\xff", 12},  // eax=[rbp-0xea0]; [rbp-0xe9c]=eax
    {0x1095511, "\xe8\x8a\x9b\x3d\x00", 5},                               // call 146f0a0 (engine)
    {0x1095516, "\x48\x8d\xb5\x64\xf1\xff\xff\x89\xda\x48\x89\xc7", 12},  // rsi=&entity ; edx=type ; rdi=engine
};

// CComponent::addStyleClass(rdi = widget, rsi = std::string*): drops an empty
// string, drops a duplicate, otherwise MOVES the string into the widget's
// class vector at +0xb0/+0xb8/+0xc0 (0x20 per std::string) and restyles.
static const TintCheck kTintStyleChecks[] = {
    {0x30550f2, "\x48\x83\x7e\x08\x00\x75\x27", 7},                       // cmp [rsi+8],0 ; jne
    {0x3055126, "\x48\x8b\xb7\xb8\x00\x00\x00\x48\x8b\xbf\xb0\x00\x00\x00", 14},
    {0x3055195, "\x48\x83\x83\xb8\x00\x00\x00\x20", 8},                   // add [rbx+0xb8], 0x20
};

// The 11 station and 5 depot carrier-class calls. All resolve to 0x30550d0.
static const uintptr_t kTintStationClassCalls[] = {
    0x10905df, 0x1090829, 0x1090848, 0x1090867, 0x1090886, 0x10908a5,
    0x10908c4, 0x10908e3, 0x1090902, 0x1090921, 0x1090940,
};
static const uintptr_t kTintDepotClassCalls[] = {
    0x1095672, 0x1096b65, 0x1096b84, 0x1096ba3, 0x1096bc2,
};
