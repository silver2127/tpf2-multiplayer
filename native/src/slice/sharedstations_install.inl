// sharedstations_install.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// InstallSharedStations (the patch of the gate above)


static void InstallSharedStations()
{
    if (FlagsSayOff("sharedstations")) {
        Log("[sharedstations] OFF (sharedstations=0 in tpf2_menu_flags.txt) -- the line editor "
            "keeps refusing another company's stations\n");
        return;
    }
    if (!BytesAre(RVA_SHAREDSTATIONS_GUARD, SHAREDSTATIONS_EXPECT,
                  sizeof(SHAREDSTATIONS_EXPECT), "sharedstations")) return;
    // Both rel32 calls inside the guarded window must resolve where the finding
    // says: a byte match that landed on some other function cannot pass.
    int32_t rel = 0;
    memcpy(&rel, SHAREDSTATIONS_EXPECT + 5, 4);
    uintptr_t target = (uintptr_t)((int64_t)RVA_SHAREDSTATIONS_GUARD + 9 + rel);
    if (target != RVA_SS_GETENGINE) {
        Log("[sharedstations] NOT installed: the call at rva=%llx resolves to %llx, not the "
            "engine accessor %llx\n", (unsigned long long)(RVA_SHAREDSTATIONS_GUARD + 4),
            (unsigned long long)target, (unsigned long long)RVA_SS_GETENGINE);
        return;
    }
    memcpy(&rel, SHAREDSTATIONS_EXPECT + 18, 4);
    target = (uintptr_t)((int64_t)RVA_SHAREDSTATIONS_GUARD + 22 + rel);
    if (target != RVA_SS_GETPLAYEROWNED) {
        Log("[sharedstations] NOT installed: the call at rva=%llx resolves to %llx, not "
            "GetComponentPtr<PlayerOwned> %llx\n",
            (unsigned long long)(RVA_SHAREDSTATIONS_GUARD + 17),
            (unsigned long long)target, (unsigned long long)RVA_SS_GETPLAYEROWNED);
        return;
    }
    // The stub, assembled here rather than in MASM because it is a jump target
    // in the middle of a function, not a call: no prologue, no frame, and it
    // must leave rbx and rsp exactly as it found them.
    //   mov  ecx, eax              ; PlayerOwned.player
    //   mov  edx, [rbx+0x28]       ; the filter's own player entity
    //   sub  rsp, 0x20             ; shadow space; rsp was 16-aligned here
    //   mov  rax, SharedStationsAllow ; call rax ; add rsp, 0x20
    //   test al, al ; jne accept
    //   mov  rax, reject ; jmp rax
    // accept:
    //   mov  rax, accept ; jmp rax
    uint8_t* stub = NearAlloc(64);
    if (!stub) {
        Log("[sharedstations] NOT installed: no page within reach of a rel32 for the stub\n");
        return;
    }
    size_t n = 0;
    const uintptr_t helper = (uintptr_t)&SharedStationsAllow;
    const uintptr_t accept = g_base + RVA_SHAREDSTATIONS_ACCEPT;
    const uintptr_t reject = g_base + RVA_SHAREDSTATIONS_REJECT;
    stub[n++] = 0x8B; stub[n++] = 0xC8;                                      // mov ecx, eax
    stub[n++] = 0x8B; stub[n++] = 0x53; stub[n++] = 0x28;                    // mov edx,[rbx+0x28]
    stub[n++] = 0x48; stub[n++] = 0x83; stub[n++] = 0xEC; stub[n++] = 0x20;  // sub rsp, 0x20
    stub[n++] = 0x48; stub[n++] = 0xB8; memcpy(stub + n, &helper, 8); n += 8;// mov rax, helper
    stub[n++] = 0xFF; stub[n++] = 0xD0;                                      // call rax
    stub[n++] = 0x48; stub[n++] = 0x83; stub[n++] = 0xC4; stub[n++] = 0x20;  // add rsp, 0x20
    stub[n++] = 0x84; stub[n++] = 0xC0;                                      // test al, al
    stub[n++] = 0x75; stub[n++] = 0x0C;                                      // jne +12 (accept)
    stub[n++] = 0x48; stub[n++] = 0xB8; memcpy(stub + n, &reject, 8); n += 8;// mov rax, reject
    stub[n++] = 0xFF; stub[n++] = 0xE0;                                      // jmp rax
    stub[n++] = 0x48; stub[n++] = 0xB8; memcpy(stub + n, &accept, 8); n += 8;// mov rax, accept
    stub[n++] = 0xFF; stub[n++] = 0xE0;                                      // jmp rax
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    if (!PatchJumpNear(g_base + RVA_SHAREDSTATIONS_SITE, stub, SHAREDSTATIONS_STEAL, nullptr)) {
        Log("[sharedstations] NOT installed: could not write the detour at rva=%llx\n",
            (unsigned long long)RVA_SHAREDSTATIONS_SITE);
        return;
    }
    g_ssOn = true;
    Log("[sharedstations] installed rva=%llx steal=%d accept=%llx reject=%llx -- in companies "
        "mode the line editor takes another company's station as a stop; ownership, edits and "
        "demolition are unchanged\n", (unsigned long long)RVA_SHAREDSTATIONS_SITE,
        SHAREDSTATIONS_STEAL, (unsigned long long)RVA_SHAREDSTATIONS_ACCEPT,
        (unsigned long long)RVA_SHAREDSTATIONS_REJECT);
}

