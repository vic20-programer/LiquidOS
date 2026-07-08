// interrupts.h — the C++ side of exception handling. isr_common_handler
// (called from isr_stubs.asm) lands here with a pointer to everything
// pushed onto the stack: our manually-saved general purpose registers,
// the vector number + error code we pushed in the stub, and the frame
// the CPU itself pushed on interrupt entry (RIP, CS, RFLAGS, RSP, SS).
//
// This file has no dependency on VGA specifics — like interpreter.h, it
// takes an injected Output interface so kernel.cpp wires it to the real
// screen.

#pragma once
#include <stdint.h>

namespace interrupts {

// Must exactly match the push order in isr_common_stub (isr_stubs.asm),
// READ IN REVERSE — the last thing pushed is at the LOWEST address, which
// is where rsp points when isr_common_handler is called, so this struct's
// first field corresponds to the LAST push in the assembly.
struct __attribute__((packed)) InterruptFrame {
    // pushed by isr_common_stub, in this order (so reversed here):
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    // pushed by the per-vector stub itself:
    uint64_t vector_number;
    uint64_t error_code;
    // pushed automatically by the CPU on interrupt entry:
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

struct Output {
    virtual void write(const char* s) = 0;
    virtual void write_hex(uint64_t value) = 0;
};

inline Output* g_output = nullptr;

inline const char* exception_name(uint64_t vector) {
    switch (vector) {
        case 0:  return "Divide Error (#DE)";
        case 1:  return "Debug (#DB)";
        case 2:  return "Non-Maskable Interrupt";
        case 3:  return "Breakpoint (#BP)";
        case 4:  return "Overflow (#OF)";
        case 5:  return "Bound Range Exceeded (#BR)";
        case 6:  return "Invalid Opcode (#UD)";
        case 7:  return "Device Not Available (#NM)";
        case 8:  return "Double Fault (#DF)";
        case 9:  return "Coprocessor Segment Overrun";
        case 10: return "Invalid TSS (#TS)";
        case 11: return "Segment Not Present (#NP)";
        case 12: return "Stack Fault (#SS)";
        case 13: return "General Protection Fault (#GP)";
        case 14: return "Page Fault (#PF)";
        case 16: return "x87 Floating Point Exception (#MF)";
        case 17: return "Alignment Check (#AC)";
        case 18: return "Machine Check (#MC)";
        case 19: return "SIMD Floating Point Exception (#XM)";
        case 20: return "Virtualization Exception (#VE)";
        case 21: return "Control Protection Exception (#CP)";
        case 30: return "Security Exception (#SX)";
        default: return "Reserved/Unknown Exception";
    }
}

// Vectors 0-31 are CPU exceptions, defined by the architecture itself.
// Of these, some are "fatal" in the sense that this kernel has no
// recovery strategy for them yet (no demand paging, no signal delivery,
// no process model to kill-and-continue) — so for now EVERY exception
// is treated as fatal: print full diagnostics and halt. This is a
// deliberate, named limitation: a real OS would often resume execution
// after some exceptions (e.g. page fault -> allocate the page -> retry).
inline void dump_and_halt(InterruptFrame* frame) {
    if (g_output == nullptr) {
        // No output configured yet (e.g. a fault happens before kernel.cpp
        // finishes setup) — just halt silently. Better than a crash loop.
        while (true) asm volatile("hlt");
    }

    Output& out = *g_output;
    out.write("\n*** KERNEL EXCEPTION ***\n");
    out.write(exception_name(frame->vector_number));
    out.write("\nVector: ");
    out.write_hex(frame->vector_number);
    out.write("  Error code: ");
    out.write_hex(frame->error_code);
    out.write("\nRIP: ");
    out.write_hex(frame->rip);
    out.write("  CS: ");
    out.write_hex(frame->cs);
    out.write("\nRSP: ");
    out.write_hex(frame->rsp);
    out.write("  RFLAGS: ");
    out.write_hex(frame->rflags);
    out.write("\nRAX: ");
    out.write_hex(frame->rax);
    out.write("  RBX: ");
    out.write_hex(frame->rbx);
    out.write("\nRCX: ");
    out.write_hex(frame->rcx);
    out.write("  RDX: ");
    out.write_hex(frame->rdx);
    out.write("\nRSI: ");
    out.write_hex(frame->rsi);
    out.write("  RDI: ");
    out.write_hex(frame->rdi);
    out.write("\nRBP: ");
    out.write_hex(frame->rbp);

    if (frame->vector_number == 14) {
        // Page faults set CR2 to the faulting virtual address — extremely
        // useful for diagnosing *what* memory access went wrong, distinct
        // from RIP (which only says *where the code was*).
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        out.write("\nCR2 (faulting address): ");
        out.write_hex(cr2);
    }

    out.write("\n\nSystem halted.\n");

    while (true) {
        asm volatile("hlt");
    }
}

} // namespace interrupts

// Called from the assembly common stub (isr_common_stub in isr_stubs.asm).
// extern "C" so the name isn't mangled — the assembly references it as a
// plain symbol, same reasoning as kmain in earlier versions of this project.
extern "C" void isr_common_handler(interrupts::InterruptFrame* frame) {
    interrupts::dump_and_halt(frame);
}
