// idt_init.h — declares the 32 assembly stub symbols from isr_stubs.asm
// and installs each one into the corresponding IDT entry. Kept separate
// from idt.h so idt.h stays generic (it knows nothing about exceptions
// specifically — just "how to set an entry") and this file is the part
// that says "vector 0 maps to isr_stub_0," etc.

#pragma once
#include "idt.h"

// Each of these is a label defined in isr_stubs.asm via the ISR_NOERRCODE/
// ISR_ERRCODE macros. extern "C" because they're assembly symbols with no
// C++ name mangling.
extern "C" {
    void isr_stub_0();  void isr_stub_1();  void isr_stub_2();  void isr_stub_3();
    void isr_stub_4();  void isr_stub_5();  void isr_stub_6();  void isr_stub_7();
    void isr_stub_8();  void isr_stub_9();  void isr_stub_10(); void isr_stub_11();
    void isr_stub_12(); void isr_stub_13(); void isr_stub_14(); void isr_stub_15();
    void isr_stub_16(); void isr_stub_17(); void isr_stub_18(); void isr_stub_19();
    void isr_stub_20(); void isr_stub_21(); void isr_stub_22(); void isr_stub_23();
    void isr_stub_24(); void isr_stub_25(); void isr_stub_26(); void isr_stub_27();
    void isr_stub_28(); void isr_stub_29(); void isr_stub_30(); void isr_stub_31();

    // IRQ stubs - vector = 32 + IRQ line (see pic.h for why 32)
    void irq_stub_0();  // IRQ0  -> vector 32 -> PIT timer
    void irq_stub_1();  // IRQ1  -> vector 33 -> keyboard

    // Software interrupt used by cooperative tasking::yield() - see
    // isr_stubs.asm and tasking.h for why this shares the IRQ path's
    // common stub rather than the CPU-exception path's.
    void isr_stub_0x80();
}

namespace idt {

inline void install_exception_handlers() {
    set_entry(0,  (uint64_t)isr_stub_0,  GATE_INTERRUPT_64BIT);
    set_entry(1,  (uint64_t)isr_stub_1,  GATE_INTERRUPT_64BIT);
    set_entry(2,  (uint64_t)isr_stub_2,  GATE_INTERRUPT_64BIT);
    set_entry(3,  (uint64_t)isr_stub_3,  GATE_INTERRUPT_64BIT);
    set_entry(4,  (uint64_t)isr_stub_4,  GATE_INTERRUPT_64BIT);
    set_entry(5,  (uint64_t)isr_stub_5,  GATE_INTERRUPT_64BIT);
    set_entry(6,  (uint64_t)isr_stub_6,  GATE_INTERRUPT_64BIT);
    set_entry(7,  (uint64_t)isr_stub_7,  GATE_INTERRUPT_64BIT);
    set_entry(8,  (uint64_t)isr_stub_8,  GATE_INTERRUPT_64BIT);
    set_entry(9,  (uint64_t)isr_stub_9,  GATE_INTERRUPT_64BIT);
    set_entry(10, (uint64_t)isr_stub_10, GATE_INTERRUPT_64BIT);
    set_entry(11, (uint64_t)isr_stub_11, GATE_INTERRUPT_64BIT);
    set_entry(12, (uint64_t)isr_stub_12, GATE_INTERRUPT_64BIT);
    set_entry(13, (uint64_t)isr_stub_13, GATE_INTERRUPT_64BIT);
    set_entry(14, (uint64_t)isr_stub_14, GATE_INTERRUPT_64BIT);
    set_entry(15, (uint64_t)isr_stub_15, GATE_INTERRUPT_64BIT);
    set_entry(16, (uint64_t)isr_stub_16, GATE_INTERRUPT_64BIT);
    set_entry(17, (uint64_t)isr_stub_17, GATE_INTERRUPT_64BIT);
    set_entry(18, (uint64_t)isr_stub_18, GATE_INTERRUPT_64BIT);
    set_entry(19, (uint64_t)isr_stub_19, GATE_INTERRUPT_64BIT);
    set_entry(20, (uint64_t)isr_stub_20, GATE_INTERRUPT_64BIT);
    set_entry(21, (uint64_t)isr_stub_21, GATE_INTERRUPT_64BIT);
    set_entry(22, (uint64_t)isr_stub_22, GATE_INTERRUPT_64BIT);
    set_entry(23, (uint64_t)isr_stub_23, GATE_INTERRUPT_64BIT);
    set_entry(24, (uint64_t)isr_stub_24, GATE_INTERRUPT_64BIT);
    set_entry(25, (uint64_t)isr_stub_25, GATE_INTERRUPT_64BIT);
    set_entry(26, (uint64_t)isr_stub_26, GATE_INTERRUPT_64BIT);
    set_entry(27, (uint64_t)isr_stub_27, GATE_INTERRUPT_64BIT);
    set_entry(28, (uint64_t)isr_stub_28, GATE_INTERRUPT_64BIT);
    set_entry(29, (uint64_t)isr_stub_29, GATE_INTERRUPT_64BIT);
    set_entry(30, (uint64_t)isr_stub_30, GATE_INTERRUPT_64BIT);
    set_entry(31, (uint64_t)isr_stub_31, GATE_INTERRUPT_64BIT);
    // Entries 32-255 are available for hardware IRQs and software
    // interrupts. We wire up IRQ0 (timer) and IRQ1 (keyboard) now;
    // IRQ2-15 (vectors 34-47) are left unset until something needs them.
    set_entry(32, (uint64_t)irq_stub_0, GATE_INTERRUPT_64BIT);
    set_entry(33, (uint64_t)irq_stub_1, GATE_INTERRUPT_64BIT);

    // Software yield interrupt - DPL doesn't actually matter here since
    // this kernel has no ring 3 code yet (everything runs in ring 0), but
    // using the same gate type as everything else keeps this consistent.
    set_entry(0x80, (uint64_t)isr_stub_0x80, GATE_INTERRUPT_64BIT);

    load();
}

} // namespace idt
