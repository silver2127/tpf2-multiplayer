// Capture module: snapshots the latest build-intent (transform + params)
// from the make_proposal chain, and emits a NetEvent when applyProposal
// commits. Target RVAs from M1/M2 recon (ASLR-safe: base + rva).
#pragma once
#include <cstdint>
#include "net.h"

bool Capture_Init(uintptr_t gameBase, void (*logFn)(const char* fmt, ...));
void Capture_SetLocalEventCb(void (*cb)(const NetEvent&));

// called from the relay stub (applyrelay.asm) — full original register set + id
extern "C" void Capture_Handler(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9, uint64_t id);
