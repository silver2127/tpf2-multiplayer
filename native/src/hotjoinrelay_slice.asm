; Relays for HOT-JOIN ORDER (slice/hotjoin_order.inl). Build 35924.
;
; Each is entered by a `jmp rel32` planted over one whole instruction just
; before the engine consumes an entity batch (the rel32 lands on a `jmp [rip+0]`
; in a page within reach of the exe, which carries the relay's address):
;
;   candidates  GetTargetsByLandUse       rva 0x927df6  c7 45 87 01 00 00 00  mov dword ptr [rbp-79h],1
;               vector<Entity> at rbp-71h (begin, end, cap)
;   departures  SimEntityAtBuilding::Update2  rva 0xa7c9fd  48 8d 54 24 28  lea rdx,[rsp+28h]
;               vector<Entity> at rsp+28h
;   arrivals    PersonMoveSystem::Update2 rva 0xa59928  48 8d 54 24 68        lea rdx,[rsp+68h]
;               vector<Entity> at rsp+68h
;   idle        SimEntityIdleSystem::Update rva 0xa867ce  49 8b 55 20 49 2b 55 18
;               mov rdx,[r13+20h] / sub rdx,[r13+18h]; the pending list at r13+18h
;               (r13 = the system; the list is the system's own, sorted in place)
;
; The relay hands HotJoinSort(site, &vector) the vector's address, restores
; every register and the flags, executes the stolen instruction itself with rsp
; back at the value the engine left, and jumps to site+steal.
;
; REGISTER SAFETY. Flags and all fourteen general registers the relay or the
; C++ callee could touch are pushed and popped; rbp is never written (the callee
; preserves it). xmm0-xmm5 are the volatile xmm registers, saved around the
; call; xmm6-xmm15 are nonvolatile for the callee. rsp is realigned for the call
; and restored exactly. At rva 0xa7c9fd / 0xa59928 rcx holds the signal the
; engine is about to call through: it survives like everything else.

EXTERN HotJoinSort:PROC
EXTERN g_hjResume0:QWORD
EXTERN g_hjResume1:QWORD
EXTERN g_hjResume2:QWORD
EXTERN g_hjResume3:QWORD

.code

; 15 pushes = 78h bytes: the engine's rsp at the site is rbx+78h below.
HotJoinBody MACRO site, vecExpr
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    mov  rbx, rsp                  ; rbx doubles as the saved rsp from here on
    and  rsp, 0FFFFFFFFFFFFFFF0h   ; 16-byte align whatever we were entered on
    sub  rsp, 80h                  ; shadow space (20h) + six xmm slots (60h)
    movaps xmmword ptr [rsp+20h], xmm0
    movaps xmmword ptr [rsp+30h], xmm1
    movaps xmmword ptr [rsp+40h], xmm2
    movaps xmmword ptr [rsp+50h], xmm3
    movaps xmmword ptr [rsp+60h], xmm4
    movaps xmmword ptr [rsp+70h], xmm5

    mov  ecx, site                 ; arg1  int site
    lea  rdx, vecExpr              ; arg2  the vector's {begin, end, cap}
    call HotJoinSort

    movaps xmm0, xmmword ptr [rsp+20h]
    movaps xmm1, xmmword ptr [rsp+30h]
    movaps xmm2, xmmword ptr [rsp+40h]
    movaps xmm3, xmmword ptr [rsp+50h]
    movaps xmm4, xmmword ptr [rsp+60h]
    movaps xmm5, xmmword ptr [rsp+70h]
    mov  rsp, rbx
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rdi
    pop  rsi
    pop  rbx
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    popfq
ENDM

HotJoinCandidatesRelay PROC
    HotJoinBody 0, [rbp-71h]
    mov  dword ptr [rbp-79h], 1    ; the stolen instruction
    jmp  qword ptr [g_hjResume0]
HotJoinCandidatesRelay ENDP

HotJoinDeparturesRelay PROC
    HotJoinBody 1, [rbx+78h+28h]
    lea  rdx, [rsp+28h]            ; the stolen instruction
    jmp  qword ptr [g_hjResume1]
HotJoinDeparturesRelay ENDP

HotJoinArrivalsRelay PROC
    HotJoinBody 2, [rbx+78h+68h]
    lea  rdx, [rsp+68h]            ; the stolen instruction
    jmp  qword ptr [g_hjResume2]
HotJoinArrivalsRelay ENDP

HotJoinIdleRelay PROC
    HotJoinBody 3, [r13+18h]
    mov  rdx, qword ptr [r13+20h]  ; the two stolen instructions
    sub  rdx, qword ptr [r13+18h]
    jmp  qword ptr [g_hjResume3]
HotJoinIdleRelay ENDP

END
