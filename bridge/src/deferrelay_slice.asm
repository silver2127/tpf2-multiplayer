; Cancel-capable relay for slice_hook, 7-argument handler.
;
; Identical contract to deferrelay.asm (0 = proceed to trampoline, 1 = suppress
; and return 0 to the game) with one addition: the handler also receives the
; hooked FUNCTION's entry rsp, so it can read stack-passed arguments. BuyVehicle
; takes a TransportVehicleConfig by value, which MSVC passes as a pointer in the
; 4th slot -- [rsp+0x28] from the callee's point of view -- and without rsp the
; handler could see only rcx/rdx/r8/r9.
;
; STACK at relay entry, from the blob's pushes:
;   [rsp]      tramp
;   [rsp+8]    original rax
;   [rsp+0x10] original r10
;   [rsp+0x18] the game's return address   <- this slot IS the callee's entry rsp
; This proc pushes 5 regs (0x28) and subtracts 0xA8, so relay-entry rsp sits at
; rsp+0xD0, the game return address at rsp+0xE8, and the callee's entry rsp is
; that same address: rsp+0xE8. Its stack args start at callee_rsp+0x28.
;
; Handler: uint64_t DeferHandler(rcx, rdx, r8, r9, id, retAddr, calleeRsp)
;   args 5..7 go on the stack at [rsp+0x20], [rsp+0x28], [rsp+0x30].

EXTERN DeferHandler:PROC

.code
DeferRelay PROC
    push rcx
    push rdx
    push r8
    push r9
    push r11
    sub  rsp, 0A8h
    movdqu [rsp+40h], xmm0
    movdqu [rsp+50h], xmm1
    movdqu [rsp+60h], xmm2
    movdqu [rsp+70h], xmm3
    movdqu [rsp+80h], xmm4
    movdqu [rsp+90h], xmm5
    mov  [rsp+20h], rax        ; 5th arg = target id
    mov  rax, [rsp+0E8h]       ; the game's return address
    mov  [rsp+28h], rax        ; 6th arg = caller
    lea  rax, [rsp+0E8h]       ; address of that slot == callee's entry rsp
    mov  [rsp+30h], rax        ; 7th arg = calleeRsp
    call DeferHandler          ; -> rax = 0 proceed / 1 suppress
    mov  [rsp+20h], rax        ; stash the decision across the xmm restores
    movdqu xmm0, [rsp+40h]
    movdqu xmm1, [rsp+50h]
    movdqu xmm2, [rsp+60h]
    movdqu xmm3, [rsp+70h]
    movdqu xmm4, [rsp+80h]
    movdqu xmm5, [rsp+90h]
    mov  rax, [rsp+20h]
    add  rsp, 0A8h
    pop  r11
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    ; [rsp]=tramp  [rsp+8]=orig_rax  [rsp+0x10]=orig_r10  [rsp+0x18]=game_ret
    test rax, rax
    jnz  SuppressIt

    mov  r10, [rsp]                 ; tramp
    mov  rax, [rsp+8]               ; restore orig rax
    xchg r10, qword ptr [rsp+10h]   ; orig-r10 slot := tramp; r10 := orig r10
    add  rsp, 18h
    jmp  qword ptr [rsp - 8]        ; -> trampoline -> original prologue

SuppressIt:
    mov  r10, [rsp+10h]             ; restore orig r10
    add  rsp, 18h                   ; rsp -> game_ret
    xor  eax, eax                   ; return 0; callers of these factories ignore it
    ret
DeferRelay ENDP
END
