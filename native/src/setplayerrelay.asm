; Build 35924: enter AFTER the binding has initialized its cleanup object.
; Reserved player range 0x60000000..0x6fffffff requests entity-only ownership.
; Ordinary calls reproduce the displaced construction dispatch exactly.
EXTERN g_setPlayerGeneric:QWORD
EXTERN g_setPlayerConstruction:QWORD
EXTERN g_setPlayerLine:QWORD
.code
SetPlayerRelay PROC
    mov eax, dword ptr [rbp+80h]
    and eax, 0f0000000h
    cmp eax, 60000000h
    jne ordinary
    and dword ptr [rbp+80h], 0fffffffh
    jmp qword ptr [g_setPlayerGeneric]
ordinary:
    test rsi, rsi
    jz lineBranch
    lea rax, [rbp+80h]
    mov qword ptr [rsp+20h], rax
    jmp qword ptr [g_setPlayerConstruction]
lineBranch:
    jmp qword ptr [g_setPlayerLine]
SetPlayerRelay ENDP
END
