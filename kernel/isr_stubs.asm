; isr_stubs.asm — the actual landing pads the CPU jumps to on interrupt.
;
; x86_64 mandates that the CPU push (in order, after RIP/CS/flags/etc are
; pushed automatically by hardware): an error code, but ONLY for certain
; exception vectors. Vectors like divide-by-zero (0) push no error code;
; vectors like general protection fault (13) push a 64-bit error code.
; Mixing these up corrupts the stack, so each stub explicitly handles its
; own case rather than trying to be "clever" and unified at this layer.
;
; Each stub: pushes a dummy error code if the CPU didn't (so the shared
; C++ side always sees the same stack shape), pushes the vector number,
; then jumps to a common handler that saves the remaining registers and
; calls into C++.

BITS 64
section .text

extern isr_common_handler

; ---------------------------------------------------------------------------
; Common handler: saves general-purpose registers, calls the C++ handler,
; restores registers, and returns from the interrupt. Every stub below
; jumps here after pushing (error_code, vector_number) onto the stack.
; ---------------------------------------------------------------------------
isr_common_stub:
    ; save general purpose registers (CPU does NOT do this for us)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; rdi is the first argument in the calling convention we use for
    ; isr_common_handler — pass it a pointer to everything we just pushed
    ; plus what the CPU pushed (error code, vector, then iret frame),
    ; so the C++ side can read/print all of it as a single struct.
    mov rdi, rsp
    call isr_common_handler

    ; restore registers (only matters for exceptions we choose to resume
    ; from — for the fatal ones in this project, isr_common_handler halts
    ; and never returns here at all)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16 ; discard our pushed (error_code, vector_number)
    iretq

; ---------------------------------------------------------------------------
; Macro-like repetition via NASM %macro — generates one stub per vector.
; NOERRCODE vectors push a dummy 0 so the stack shape always matches
; ERRCODE vectors, which push the CPU's real error code.
; ---------------------------------------------------------------------------
%macro ISR_NOERRCODE 1
global isr_stub_%1
isr_stub_%1:
    push 0      ; dummy error code
    push %1     ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr_stub_%1
isr_stub_%1:
    ; CPU already pushed the real error code for this vector
    push %1     ; vector number
    jmp isr_common_stub
%endmacro

; CPU exception vectors 0-31 (see Intel SDM Vol 3, Chapter 6 for the
; canonical list — these names/numbers are architecture-defined, not
; something we get to choose)
ISR_NOERRCODE 0   ; #DE Divide Error
ISR_NOERRCODE 1   ; #DB Debug
ISR_NOERRCODE 2   ; NMI
ISR_NOERRCODE 3   ; #BP Breakpoint
ISR_NOERRCODE 4   ; #OF Overflow
ISR_NOERRCODE 5   ; #BR Bound Range Exceeded
ISR_NOERRCODE 6   ; #UD Invalid Opcode
ISR_NOERRCODE 7   ; #NM Device Not Available
ISR_ERRCODE   8   ; #DF Double Fault
ISR_NOERRCODE 9   ; Coprocessor Segment Overrun (legacy, unused on x86_64)
ISR_ERRCODE   10  ; #TS Invalid TSS
ISR_ERRCODE   11  ; #NP Segment Not Present
ISR_ERRCODE   12  ; #SS Stack Fault
ISR_ERRCODE   13  ; #GP General Protection Fault
ISR_ERRCODE   14  ; #PF Page Fault
ISR_NOERRCODE 15  ; reserved
ISR_NOERRCODE 16  ; #MF x87 Floating Point Exception
ISR_ERRCODE   17  ; #AC Alignment Check
ISR_NOERRCODE 18  ; #MC Machine Check
ISR_NOERRCODE 19  ; #XM SIMD Floating Point Exception
ISR_NOERRCODE 20  ; #VE Virtualization Exception
ISR_ERRCODE   21  ; #CP Control Protection Exception
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30  ; #SX Security Exception
ISR_NOERRCODE 31  ; reserved

; ---------------------------------------------------------------------------
; IRQ stubs (vectors 32-47, after PIC remap in pic.h). Unlike CPU exception
; stubs above, these call a DIFFERENT common handler (irq_common_stub) that
; is expected to RETURN — a timer tick or keystroke is normal operation,
; not a fatal error, so execution must resume exactly where it left off.
; ---------------------------------------------------------------------------

; ---------------------------------------------------------------------------
; IRQ stubs (vectors 32-47, after PIC remap in pic.h), AND the software
; "yield" interrupt (vector 0x80, used by cooperative tasking::yield()).
; All of these share ONE common stub. Unlike the CPU exception stubs
; above, this one is expected to RESUME EXECUTION SOMEWHERE — either back
; into the SAME task it interrupted (the common case: a timer tick where
; nothing needs to switch), or into a DIFFERENT task entirely (preemption,
; or a deliberate yield).
;
; THE KEY DESIGN POINT (see tasking.h for the full explanation): every
; task's saved/resumable state is made to look EXACTLY like "a stack
; that's paused right here, mid-interrupt, about to pop registers and
; iretq." This is true whether that task was paused by a real timer IRQ
; firing on it, or by it cooperatively calling yield() (which just
; triggers THIS SAME stub via a software `int 0x80`, rather than the CPU
; raising it on its own). Because every saved task looks identical in
; shape, switching to a different one is just: swap RSP to that task's
; saved value, then fall through into the exact same pop-registers-and-
; iretq tail every path already uses. No separate "two kinds of saved
; state" to keep consistent.
; ---------------------------------------------------------------------------

extern irq_common_handler
extern tasking_maybe_switch ; C++ function: may change rsp via the pointer it's given

global irq_common_stub
irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; First, let the timer/keyboard C++ handler do its normal work
    ; (incrementing the tick counter, reading a scancode, etc) exactly as
    ; before - this does NOT switch tasks itself.
    mov rdi, rsp
    call irq_common_handler

    ; THEN, separately, give the task scheduler a chance to switch stacks.
    ; tasking_maybe_switch takes the CURRENT rsp (pointing at everything
    ; we just pushed, plus the hardware iretq frame below it), decides
    ; whether a different task should run next, and returns EITHER the
    ; same rsp unchanged (no switch - keep resuming this task) or a
    ; DIFFERENT task's previously-saved rsp (switch - resume that one
    ; instead). Either way, what comes back is guaranteed to point at the
    ; same [15 registers][error_code][vector][iretq frame] shape, because
    ; that's the only shape any saved task is ever allowed to have.
    mov rdi, rsp
    call tasking_maybe_switch
    mov rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16 ; discard the (error_code placeholder, vector_number) pair
    iretq

%macro IRQ_STUB 2
global irq_stub_%1
irq_stub_%1:
    push 0      ; dummy error code, IRQs never have one (kept for identical
                ; stack shape vs the exception path, so InterruptFrame's
                ; layout in interrupts.h works for both)
    push %2     ; vector number (32 + IRQ line)
    jmp irq_common_stub
%endmacro

; IRQ0 = vector 32 = PIT timer. IRQ1 = vector 33 = keyboard.
; Only these two are wired up for now; IRQ2-15 (vectors 34-47) can be
; added the same way later (mouse, secondary ATA, etc).
IRQ_STUB 0, 32
IRQ_STUB 1, 33

; ---------------------------------------------------------------------------
; Software interrupt vector 0x80: deliberately triggered by `int 0x80`
; (see tasking.h's yield()) rather than raised by hardware. This is what
; lets COOPERATIVE yielding go through the EXACT SAME stub as preemption
; — a task calling yield() ends up parked in the identical stack shape as
; a task that got preempted by the timer, so the scheduler never has to
; treat the two cases differently. irq_common_handler (the C++ timer/
; keyboard dispatcher) explicitly ignores vector 0x80 - there's no
; hardware device behind it, so there's nothing for it to do besides let
; tasking_maybe_switch make the actual switching decision below.
; ---------------------------------------------------------------------------
global isr_stub_0x80
isr_stub_0x80:
    push 0      ; dummy error code - this is a software interrupt, no
                ; hardware-supplied error code exists for it
    push 0x80   ; vector number
    jmp irq_common_stub

