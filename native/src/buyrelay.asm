; Relay for the buyVehicle command-factory hook (RVA 0x9dca00).
;
; CRITICAL: the stolen prologue starts with `mov rax, rsp` and then
; `lea rbp, [rax - 0ad8h]`, i.e. the function derives its frame pointer from
; the stack pointer AS IT WAS AT ENTRY. Our pushes must therefore be exactly
; balanced before jumping to the trampoline, or rbp lands somewhere wrong and
; the function corrupts its own frame.
;
; Alignment: entry rsp = 8 (mod 16); 7 pushes -> 0 (mod 16); sub 20h keeps it
; 0 (mod 16), which is what the ABI wants at a call site.

EXTERN BuyHandler:PROC
EXTERN g_buyTramp:QWORD
EXTERN g_buyEntrySp:QWORD

.code
BuyRelay PROC
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub  rsp, 20h                  ; shadow space

    ; Stack arguments (the 5th onward) live at entry_rsp+28h. If rcx is a
    ; hidden struct-return pointer then the real config is over there, not in
    ; a register. 7 pushes (38h) + sub 20h = 58h below entry.
    mov  rax, rsp
    add  rax, 58h
    mov  qword ptr [g_buyEntrySp], rax

    mov  rcx, qword ptr [rsp+48h]  ; original rcx
    mov  rdx, qword ptr [rsp+40h]  ; original rdx  (the vehicle config)
    mov  r8,  qword ptr [rsp+38h]  ; original r8
    mov  r9,  qword ptr [rsp+30h]  ; original r9
    call BuyHandler

    add  rsp, 20h
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    jmp  qword ptr [g_buyTramp]    ; -> stolen prologue -> rest of the factory
BuyRelay ENDP
END
