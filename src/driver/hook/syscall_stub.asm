; blook-drv-lite - syscall probe stub.
;
; Entered from the generated trampoline after it replayed the KiSystemCall64
; prologue up to the service dispatch. RSP therefore points at the completed
; trap frame, exactly like the original `mov rax, r10; call rax` would have.
;
; The stub saves every general purpose register, fills a probe context and
; calls drvlite_syscall_probe. It then either:
;   * chains: jumps back to the real `call rax` dispatch, or
;   * handled: puts the callback status in RAX and resumes after the dispatch
;     (this is what implements the syscall shield and the debug-register cloak).
;
; XMM registers do not need preserving: the entry saved the user state into the
; trap frame before this point.

OPTION CASEMAP:NONE

EXTERN drvlite_syscall_probe:PROC
EXTERN g_drvlite_infinity_resume:QWORD
EXTERN g_drvlite_infinity_skip:QWORD

.code

drvlite_syscall_entry_stub PROC
    pushfq
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    sub rsp, 88h

    ; probe context at [rsp+20h]
    mov rax, gs:[188h]                 ; KTHREAD
    mov eax, dword ptr [rax+80h]       ; SystemCallNumber
    mov dword ptr [rsp+20h], eax
    mov rax, [rsp+0B0h]                ; saved r10 = service function
    mov [rsp+28h], rax
    mov rax, [rsp+0E0h]                ; saved rcx = arg0
    mov [rsp+30h], rax
    mov rax, [rsp+0D8h]                ; saved rdx = arg1
    mov [rsp+38h], rax
    mov rax, [rsp+0C0h]                ; saved r8 = arg2
    mov [rsp+40h], rax
    mov rax, [rsp+0B8h]                ; saved r9 = arg3
    mov [rsp+48h], rax
    lea rax, [rsp+100h]                ; call-site stack pointer
    mov [rsp+50h], rax
    mov qword ptr [rsp+58h], 0         ; handled + status

    sti
    lea rcx, [rsp+20h]
    call drvlite_syscall_probe
    cli

    mov eax, dword ptr [rsp+58h]       ; handled?
    test eax, eax
    jnz handled

    add rsp, 88h
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    popfq
    jmp qword ptr [g_drvlite_infinity_resume]

handled:
    mov eax, dword ptr [rsp+5Ch]       ; callback status
    mov [rsp+0F0h], rax                ; deliver it through the saved RAX slot
    add rsp, 88h
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    popfq
    jmp qword ptr [g_drvlite_infinity_skip]

drvlite_syscall_entry_stub ENDP

end
