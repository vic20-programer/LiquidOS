// pic.h — driver for the 8259 Programmable Interrupt Controller (PIC).
//
// There are two PICs chained together on real PC hardware (and faithfully
// emulated by QEMU): a "master" handling IRQ0-7 and a "slave" handling
// IRQ8-15, with the slave's output wired into the master's IRQ2 line.
//
// THE PROBLEM THIS FILE SOLVES: by default, after a hardware reset, the
// PIC is configured to deliver IRQ0-7 as CPU interrupt vectors 0x08-0x0F
// and IRQ8-15 as vectors 0x70-0x77. Vectors 0x08-0x0F are EXACTLY the
// range we just wired up for CPU exceptions (0x08 = Double Fault,
// 0x0E = Page Fault, etc in idt_init.h). If we enabled interrupts without
// remapping first, a timer tick (IRQ0) would look IDENTICAL to a double
// fault to our IDT, and our fault handler would fire — incorrectly,
// constantly, and catastrophically.
//
// THE FIX: reprogram both PICs (via their command/data I/O ports) to
// deliver IRQs starting at vector 0x20 (32) instead, safely past the
// 0-31 range reserved for CPU exceptions. This is "remapping."

#pragma once
#include <stdint.h>

namespace pic {

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// A short delay needed between successive writes to the same PIC port —
// some real (and emulated) PIC hardware can't keep up with back-to-back
// I/O at full CPU speed. Writing to an unused port is a classic, widely
// used trick to "waste" a small, consistent amount of time.
inline void io_wait() {
    outb(0x80, 0);
}

constexpr uint16_t PIC1_COMMAND = 0x20; // master PIC
constexpr uint16_t PIC1_DATA    = 0x21;
constexpr uint16_t PIC2_COMMAND = 0xA0; // slave PIC
constexpr uint16_t PIC2_DATA    = 0xA1;

constexpr uint8_t PIC_EOI = 0x20; // "End Of Interrupt" command

// New vector base for remapped IRQs. IRQ0 (timer) -> vector 0x20 (32),
// IRQ1 (keyboard) -> vector 0x21 (33), and so on up through IRQ15 -> 0x2F.
constexpr uint8_t IRQ_BASE = 0x20;

// Remaps both PICs so hardware IRQs land on vectors 0x20-0x2F instead of
// the default 0x08-0x0F / 0x70-0x77. This is the standard ICW
// (Initialization Command Word) sequence from the Intel 8259 datasheet —
// the four ICWs below are not arbitrary, they're the documented protocol.
inline void remap() {
    uint8_t mask1 = inb(PIC1_DATA); // preserve currently-masked IRQs
    uint8_t mask2 = inb(PIC2_DATA);

    // ICW1: start initialization sequence, in cascade mode
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    // ICW2: vector offset for each PIC
    outb(PIC1_DATA, IRQ_BASE);       // master: IRQ0-7 -> 0x20-0x27
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);   // slave:  IRQ8-15 -> 0x28-0x2F
    io_wait();

    // ICW3: tell each PIC how they're wired to each other
    outb(PIC1_DATA, 0x04); // master: slave PIC is connected to IRQ2 (bit 2)
    io_wait();
    outb(PIC2_DATA, 0x02); // slave: "I am connected to master's IRQ2"
    io_wait();

    // ICW4: set 8086/88 mode (the only mode relevant to us)
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    // restore the IRQ masks that were in effect before remapping
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

// Unmasks (enables) a specific IRQ line. IRQ 0-7 are on the master PIC,
// 8-15 on the slave; masking is per-bit in each PIC's data port.
inline void unmask_irq(uint8_t irq_line) {
    uint16_t port;
    uint8_t value;

    if (irq_line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq_line -= 8;
    }

    value = inb(port) & ~(1 << irq_line); // clear the bit to unmask
    outb(port, value);
}

// Masks (disables) a specific IRQ line — the inverse of unmask_irq().
// Needed for IRQ lines a device can still raise even when nothing
// listens for them via interrupts: e.g. the ATA controller (IRQ14)
// asserts its interrupt line whenever a PIO command completes, whether
// or not the driver is using interrupts to notice — and SeaBIOS leaves
// IRQ14 unmasked after boot for its own legacy disk access, so without
// an explicit mask_irq(14) a polling-only ATA driver (see ata.h) would
// have its first read deliver vector 46 to an IDT entry nothing installed,
// straight into a #GP.
inline void mask_irq(uint8_t irq_line) {
    uint16_t port;
    uint8_t value;

    if (irq_line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq_line -= 8;
    }

    value = inb(port) | (1 << irq_line); // set the bit to mask
    outb(port, value);
}

// Must be called at the end of every IRQ handler — tells the PIC "I'm
// done handling this interrupt, you can deliver the next one." Without
// this, the PIC thinks the IRQ line is still being serviced and won't
// deliver further interrupts on that line (or, depending on PIC mode,
// on any line at or below it).
inline void send_eoi(uint8_t irq_line) {
    if (irq_line >= 8) {
        outb(PIC2_COMMAND, PIC_EOI); // slave needs its own EOI too
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

} // namespace pic
