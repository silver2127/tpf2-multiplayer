; Relay for the train reservation-order patch (slice_hook.cpp, "TRAIN
; RESERVATION ORDER"). Build 35924.
;
; Entered by the 14-byte jmp [rip+0] planted over 16 bytes at RVA 0xabe02d,
; inside ecs::TrainMoveSystem::Update2, where the engine reads the game-time
; value it seeds its shuffle with:
;
;   abe02d  48 8b 46 48     mov rax,[rsi+48h]      ; rsi = TrainMoveSystem*
;   abe031  48 8b 48 18     mov rcx,[rax+18h]
;   abe035  e8 86 97 7c ff  call 2877c0            ; -> int at GameTime+0x30
;   abe03a  44 8b c0        mov r8d,eax            ; the seed
;   abe03d  ...                                    ; seed fixup, then the shuffle
;   abe194  48 8b 74 24 78  mov rsi,[rsp+78h]      ; <- we resume here
;   abe199  48 8b 4d e8     mov rcx,[rbp-18h]
;
; There is NO return path and no trampoline. The relay orders the array itself
; and jumps PAST the engine's Fisher-Yates to 0xabe194, so the seed never
; reaches r8d and the engine's shuffle never runs. (A trampoline could not work
; anyway -- the stolen bytes contain a call rel32, which hook.cpp copies
; verbatim and which would then point into space.)
;
; REGISTERS. Every register the relay touches is pushed and popped, so the only
; change the engine can see is the CONTENTS of the index array rdi points at,
; which is the point of the patch. What the engine needs to survive this and
; does:
;   rdi  the idx array base            - saved/restored (and re-read from
;                                        [rbp-0x20] by the engine anyway)
;   rbp  frame pointer                 - never written; [rbp-0x18] (idx end)
;                                        and [rbp-0x20] (idx base) still valid
;   rsi  TrainMoveSystem* `this`       - saved/restored, and reloaded from
;                                        [rsp+0x78] by the first instruction
;                                        we resume on
;   rbx  the train count n             - saved/restored (used as our rsp save)
;   r13  the ECS world, Update2's 2nd argument, read at 0xabe2d0
;                                      - saved/restored
;   r15  zero, and r14/r11/r10/r9 are dead here (the engine recomputed them
;        inside the shuffle we skip) - saved anyway, cheaper than being wrong
;   xmm6 live since abdc60             - never touched here, and nonvolatile
;                                        for the C++ callee
;   rsp  restored exactly, including the realignment
; rcx is NOT preserved as a value the engine needs: it is reloaded at 0xabe199.

EXTERN TrainOrderFix:PROC
EXTERN g_trainOrderResume:QWORD

.code
TrainOrderRelay PROC
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
    and  rsp, 0FFFFFFFFFFFFFFF0h   ; 16-byte align whatever we were called on
    sub  rsp, 30h                  ; shadow space + the 5th argument's slot

    mov  rcx, rdi                  ; arg1  int32_t* idx    (idx array base)
    mov  rdx, [rbx+30h]            ; arg2  int64_t n       (the engine's rbx)
    mov  r8,  [rbp-18h]            ; arg3  int32_t* idxEnd (cross-check for n)
    mov  r9,  rsi                  ; arg4  void* self      (TrainMoveSystem*)
    mov  rax, [rbx+10h]            ; the engine's r13
    mov  [rsp+20h], rax            ; arg5  void* world     (the ECS world)
    call TrainOrderFix

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
    jmp  qword ptr [g_trainOrderResume]
TrainOrderRelay ENDP
END
