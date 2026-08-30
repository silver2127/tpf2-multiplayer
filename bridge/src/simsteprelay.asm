; Relay for the GameSim::Step hook.
;
; The patch at GameSim::Step is a plain `jmp [rip+0]`, so on entry the stack is
; exactly as the game left it at function entry: [rsp] = return address, nothing
; pushed by us. That makes this much simpler than probe3's blob+relay.
;
; What must be preserved: only the incoming argument registers. Everything else
; volatile (rax, r10, r11, xmm0-5) is already fair game for Step to clobber, so
; there is no reason to spend cycles saving it -- and this runs every tick.
; Non-volatiles are preserved by the C handler under the normal ABI.
;
; Alignment: at entry rsp = 8 (mod 16). Five pushes -> 0 (mod 16);
; sub 20h -> still 0 (mod 16), which is what the ABI wants at a call site.

EXTERN SimStepHandler:PROC
EXTERN g_simStepTramp:QWORD

.code
SimStepRelay PROC
    push rax
    push rcx
    push rdx
    push r8
    push r9
    sub  rsp, 20h                  ; shadow space

    mov  rcx, qword ptr [rsp+38h]  ; saved rcx = GameSim*   -> arg1
    mov  rdx, qword ptr [rsp+30h]  ; saved rdx = frameTime  -> arg2
    call SimStepHandler

    add  rsp, 20h
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    jmp  qword ptr [g_simStepTramp]   ; -> stolen prologue -> rest of Step
SimStepRelay ENDP
END
