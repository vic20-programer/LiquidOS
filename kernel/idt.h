// idt.h — Interrupt Descriptor Table setup for x86_64.
//
// The IDT is a table of 256 entries. Each entry tells the CPU: "if
// interrupt/exception number N happens, jump to this address." Without
// this table installed, any CPU exception (divide by zero, page fault,
// invalid opcode, etc.) has nowhere defined to go — the CPU triple-faults,
// which on real hardware means an instant reboot, and in QEMU usually
// means the emulator just resets or halts with no diagnostic.
//
// Each IDT entry also references a segment selector from the GDT (we
// reuse the 64-bit code segment already set up in boot.asm) and some
// flags (present bit, privilege level, gate type).
//
// IMPORTANT ARCHITECTURAL NOTE: the CPU jumps to raw addresses on
// interrupt — it does NOT know about C++ calling conventions, doesn't
// save registers for you (beyond a minimal hardware-pushed frame), and
// for some exceptions pushes an extra "error code" on the stack that
// others don't. This is why every handler has a hand-written assembly
// "stub" (see isr_stubs.asm) that normalizes all of this into a single
// consistent C++ function signature before calling into interrupt.h.

#pragma once
#include <stdint.h>

namespace idt {

// One entry in the IDT — this exact 16-byte layout is mandated by the
// x86_64 architecture, not something we get to choose.
struct __attribute__((packed)) IdtEntry {
    uint16_t offset_low;    // bits 0-15 of handler address
    uint16_t selector;      // code segment selector (from GDT)
    uint8_t  ist;           // interrupt stack table offset (0 = don't switch stacks)
    uint8_t  type_attr;     // gate type + privilege level + present bit
    uint16_t offset_mid;    // bits 16-31 of handler address
    uint32_t offset_high;   // bits 32-63 of handler address
    uint32_t zero;          // reserved, must be 0
};

struct __attribute__((packed)) IdtPointer {
    uint16_t limit;
    uint64_t base;
};

constexpr int IDT_ENTRIES = 256;
constexpr uint8_t GATE_INTERRUPT_64BIT = 0x8E; // present, ring 0, 64-bit interrupt gate
constexpr uint16_t KERNEL_CODE_SELECTOR = 0x08; // matches gdt64.code in boot.asm (index 1 * 8)

inline IdtEntry g_idt[IDT_ENTRIES];
inline IdtPointer g_idt_pointer;

inline void set_entry(int index, uint64_t handler_addr, uint8_t type_attr) {
    IdtEntry& e = g_idt[index];
    e.offset_low  = (uint16_t)(handler_addr & 0xFFFF);
    e.selector    = KERNEL_CODE_SELECTOR;
    e.ist         = 0;
    e.type_attr   = type_attr;
    e.offset_mid  = (uint16_t)((handler_addr >> 16) & 0xFFFF);
    e.offset_high = (uint32_t)((handler_addr >> 32) & 0xFFFFFFFF);
    e.zero        = 0;
}

// Loads the IDT register (lidt instruction) so the CPU actually starts
// using the table we've built. Must be called after all entries are set.
inline void load() {
    g_idt_pointer.limit = sizeof(g_idt) - 1;
    g_idt_pointer.base = (uint64_t)&g_idt[0];
    asm volatile("lidt %0" : : "m"(g_idt_pointer));
}

} // namespace idt
