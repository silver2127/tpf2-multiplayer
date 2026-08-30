; PROBE-ONLY relay. Identical to applyrelay.asm except it passes the game's
; return address to the handler as a 6th argument.
;
; Needed because RtlCaptureStackBackTrace returns ZERO frames from inside a
; detour: x64 stack walking is table-driven and this relay lives in
; VirtualAlloc'd memory with no RUNTIME_FUNCTION unwind data, so the walk cannot
; leave frame 0. The return address has to be read from the stack directly.
;
; OFFSET: at relay entry the stack is [rsp]=tramp, +8=orig_rax, +0x10=orig_r10,
; +0x18=game_ret. This proc then pushes 5 registers (0x28) and subtracts 0x98,
; so entry-rsp is at rsp+0xC0 and game_ret at rsp+0xC0+0x18 = rsp+0xD8.
;
; Kept separate from applyrelay.asm so the live bridge's relay stays
; byte-identical -- that one is shared with capture.cpp and is not worth
; disturbing to answer a question.
; Shared relay for per-target apply-function hooks (probe3).
; Each hooked target jumps to a generated 33-byte blob:
;   push r10 / push rax / mov eax,<id> / mov r10,<tramp> / push r10 /
;   mov r10,<ApplyRelay> / jmp r10
; So at relay entry (top->): tramp, orig_rax, orig_r10, game_ret;
; rax = target id. Tramp is patched into the blob after InstallHook.

EXTERN ApplyHandlerProbe:PROC

.code
ApplyRelayProbe PROC
    push rcx
    push rdx
    push r8
    push r9
    push r11
    sub  rsp, 98h
    movdqu [rsp+30h], xmm0
    movdqu [rsp+40h], xmm1
    movdqu [rsp+50h], xmm2
    movdqu [rsp+60h], xmm3
    movdqu [rsp+70h], xmm4
    movdqu [rsp+80h], xmm5
    mov  [rsp+20h], rax        ; 5th arg = target id
    mov  rax, [rsp+0D8h]       ; game return address (see offset note below)
    mov  [rsp+28h], rax        ; 6th arg = caller RVA source
    call ApplyHandlerProbe
    movdqu xmm0, [rsp+30h]
    movdqu xmm1, [rsp+40h]
    movdqu xmm2, [rsp+50h]
    movdqu xmm3, [rsp+60h]
    movdqu xmm4, [rsp+70h]
    movdqu xmm5, [rsp+80h]
    add  rsp, 98h
    pop  r11
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    ; [rsp]=tramp  [rsp+8]=orig_rax  [rsp+0x10]=orig_r10  [rsp+0x18]=game_ret
    mov  r10, [rsp]            ; tramp
    mov  rax, [rsp+8]          ; restore orig rax
    xchg r10, qword ptr [rsp+10h]  ; orig-r10 slot := tramp; r10 := orig r10
    add  rsp, 18h
    jmp  qword ptr [rsp - 8]     ; -> trampoline -> original prologue -> function
ApplyRelayProbe ENDP
END
