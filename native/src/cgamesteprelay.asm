; Capture-only relay for CGame::Step (RVA 0x118e90), the main thread's
; per-frame entry that decides when the next sim batch is due. Same shape as
; simsteprelay.asm: save the argument registers, hand `this` (rcx) to the C
; handler (which adjusts the batch interval), restore, run the stolen prologue.

EXTERN CGameStepSeen:PROC
EXTERN g_cgameStepTramp:QWORD

.code
CGameStepRelay PROC
    push rax
    push rcx
    push rdx
    push r8
    push r9
    sub  rsp, 20h                  ; shadow space

    mov  rcx, qword ptr [rsp+38h]  ; saved rcx = CGame*   -> arg1
    call CGameStepSeen

    add  rsp, 20h
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    jmp  qword ptr [g_cgameStepTramp]
CGameStepRelay ENDP
END
