; Capture-only relay for the UI::CGameUI per-frame update (RVA 0x5741d0).
;
; Same shape as cgamesteprelay.asm: the patch is a plain `jmp [rip+0]`, so on
; entry the stack is exactly as the game left it. We only want `this` (rcx),
; to be able to call CGameUI::AutoSave / poke its autosave timer later, so the
; argument registers are saved, the C handler records rcx, everything is put
; back and the stolen prologue runs from the trampoline.

EXTERN GameUiSeen:PROC
EXTERN g_gameUiTramp:QWORD

.code
GameUiRelay PROC
    push rax
    push rcx
    push rdx
    push r8
    push r9
    sub  rsp, 20h                  ; shadow space

    mov  rcx, qword ptr [rsp+38h]  ; saved rcx = CGameUI*   -> arg1
    call GameUiSeen

    add  rsp, 20h
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    jmp  qword ptr [g_gameUiTramp]
GameUiRelay ENDP
END
