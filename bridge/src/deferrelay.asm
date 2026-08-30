; Relay that can CANCEL the hooked function instead of running it.
;
; Every other relay in this project observes and then always jumps to the
; trampoline, so the original runs. Lockstep needs the opposite for a player's
; own command: intercept it, keep it out of the local world, and re-issue it
; later at a stamp every peer agrees on. Applying locally and shipping a copy is
; what the old state-diff design did, and why the two worlds were never in step.
;
; The handler returns:
;   0 = proceed  -> jump to the trampoline, original executes (observe-only)
;   1 = suppress -> return straight to the game, original never runs
;
; Suppression is safe at this specific target because the caller does not read
; the return value: after `call rax` at 0x40b7d4 the next instruction is
; `cmp byte ptr [rbx],0`, which touches memory and not rax (see
; docs/re/UI_CAPTURE_PATH.md). So a plain `xor eax,eax / ret` is enough; there is
; no result to fabricate.
;
; STACK, at relay entry, from the blob's pushes:
;   [rsp]      tramp
;   [rsp+8]    original rax
;   [rsp+0x10] original r10
;   [rsp+0x18] the game's return address
; This proc then pushes 5 registers (0x28) and subtracts 0x98, so entry-rsp sits
; at rsp+0xC0 and the game return address at rsp+0xD8.

EXTERN DeferHandler:PROC

.code
DeferRelay PROC
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
    mov  rax, [rsp+0D8h]       ; the game's return address
    mov  [rsp+28h], rax        ; 6th arg = caller, so the handler can filter
    call DeferHandler          ; -> rax = 0 proceed / 1 suppress
    mov  [rsp+20h], rax        ; stash the decision; xmm restores clobber rax-adjacent slots
    movdqu xmm0, [rsp+30h]
    movdqu xmm1, [rsp+40h]
    movdqu xmm2, [rsp+50h]
    movdqu xmm3, [rsp+60h]
    movdqu xmm4, [rsp+70h]
    movdqu xmm5, [rsp+80h]
    mov  rax, [rsp+20h]        ; decision back into rax (survives the pops below)
    add  rsp, 98h
    pop  r11
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    ; [rsp]=tramp  [rsp+8]=orig_rax  [rsp+0x10]=orig_r10  [rsp+0x18]=game_ret
    test rax, rax
    jnz  SuppressIt

    ; ---- proceed: identical to applyrelay.asm ----
    mov  r10, [rsp]                 ; tramp
    mov  rax, [rsp+8]               ; restore orig rax
    xchg r10, qword ptr [rsp+10h]   ; orig-r10 slot := tramp; r10 := orig r10
    add  rsp, 18h
    jmp  qword ptr [rsp - 8]        ; -> trampoline -> original prologue

SuppressIt:
    ; Drop tramp/orig_rax/orig_r10 so rsp lands on the game's return address,
    ; then return as though the function had run and produced nothing.
    mov  r10, [rsp+10h]             ; restore orig r10
    add  rsp, 18h                   ; rsp -> game_ret
    xor  eax, eax                   ; return 0; the caller ignores it
    ret
DeferRelay ENDP
END
