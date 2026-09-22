; Relays for the ship and aircraft claim-order observers (slice_hook.cpp,
; "SHIP AND AIRCRAFT CLAIM ORDER"). Build 35924.
;
; Entered by a 5-byte `jmp rel32` planted over the first instruction of
;   ecs::ShipMoveSystem::Update2      RVA 0xa6c1e0
;   ecs::AircraftMoveSystem::Update2  RVA 0xa2bc60
; (the rel32 lands on a `jmp [rip+0]` in a page within reach of the exe, which
; carries the full address of the relay). Both functions open the same way:
;
;   48 8b c4    mov rax,rsp        ; <- the three instructions the patch steals
;   55          push rbp
;   53          push rbx
;   56          push rsi           ; <- resume here (site+5)
;   ...
;   48 8d a8 xx xx xx xx   lea rbp,[rax-0x538 / 0x588]
;
; `mov rax,rsp` is why there is no trampoline of the usual kind: everything the
; function does with rbp and its saved xmm registers hangs off rax, which must
; be the rsp the CALLER left -- the value at the first byte of the function.
; So the relay saves what it needs, does its reading, restores everything,
; and only then executes those three instructions itself with rsp back at
; exactly that value, and jumps to site+5. The engine cannot tell.
;
; ARGUMENTS at entry (void Update2(ecs::Engine*, int, float) const, so):
;   rcx   this   (ShipMoveSystem* / AircraftMoveSystem*)
;   rdx   the ECS engine ("world")
;   r8d   n, the family node count (the caller divides the node vector's byte
;         span by 20 and hands it over)
;   xmm3  dt
; The observer is handed kind, rcx, rdx and r8d. It only READS.
;
; REGISTER SAFETY. All fourteen general registers the relay or the C++ callee
; could touch are pushed and popped; rbp is never written (the callee
; preserves it, it is nonvolatile). xmm0-xmm5 are the volatile xmm registers
; and xmm3 carries dt, so all six are saved around the call; xmm6-xmm15 are
; nonvolatile for the callee and untouched here. rsp is realigned for the call
; and restored exactly. Flags are not part of a function's entry contract.

EXTERN MoveOrderObserve:PROC
EXTERN g_shipOrderResume:QWORD
EXTERN g_airOrderResume:QWORD

.code

; ecx = kind on entry to the body below (0 ship, 1 aircraft); the two public
; entry points differ only in that constant and the resume slot.
MoveOrderBody MACRO kind, resume
    push rax                       ; 14 pushes: offsets from the saved rsp below
    push rcx                       ;   rax +68  rcx +60  rdx +58  r8  +50
    push rdx                       ;   r9  +48  r10 +40  r11 +38  rbx +30
    push r8                        ;   rsi +28  rdi +20  r12 +18  r13 +10
    push r9                        ;   r14 +08  r15 +00
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

    mov  ecx, kind                 ; arg1  int kind
    mov  rdx, [rbx+60h]            ; arg2  void* self   (the engine's rcx)
    mov  r8,  [rbx+58h]            ; arg3  void* world  (the engine's rdx)
    mov  r9d, dword ptr [rbx+50h]  ; arg4  int n        (the engine's r8d)
    call MoveOrderObserve

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
    ; rsp is the function's entry value again: the return address is at [rsp]
    ; and every register holds what the caller put in it. Now the stolen three:
    mov  rax, rsp
    push rbp
    push rbx
    jmp  qword ptr [resume]
ENDM

ShipOrderRelay PROC
    MoveOrderBody 0, g_shipOrderResume
ShipOrderRelay ENDP

AirOrderRelay PROC
    MoveOrderBody 1, g_airOrderResume
AirOrderRelay ENDP

END
