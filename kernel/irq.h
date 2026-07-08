// irq.h — C++ side of hardware interrupt (IRQ) handling. Dispatches by
// vector number to whichever subsystem cares (timer tick counter,
// keyboard buffer, etc), then sends the PIC an End-Of-Interrupt so future
// interrupts keep flowing.
//
// Contrast with interrupts.h (CPU exceptions): those are fatal and never
// return. IRQs are normal, expected, frequent events — the handler here
// MUST return promptly, since it runs with interrupts effectively paused
// and a slow handler means dropped or delayed future ticks/keystrokes.

#pragma once
#include "interrupts.h" // reuses the same InterruptFrame layout
#include "pic.h"
#include <stdint.h>

namespace irq {

// Monotonically increasing counter, incremented once per timer tick.
// This is the simplest possible "clock" a kernel can have: nothing reads
// real time from it, it just counts ticks since boot. At a known PIT
// frequency (see kernel.cpp, set to 100 Hz), this is also directly how
// the cursor blink timing and (eventually) a scheduler's time-slicing
// will be driven.
inline volatile uint64_t g_tick_count = 0;

// Function pointer the keyboard subsystem registers so a keystroke can be
// handled immediately when IRQ1 fires, rather than only being noticed the
// next time something happens to poll port 0x60. kernel.cpp wires this to
// push scancodes into a small ring buffer that the main loop drains.
using KeyboardIrqCallback = void(*)(uint8_t scancode);
inline KeyboardIrqCallback g_keyboard_callback = nullptr;

inline void handle_timer() {
    g_tick_count++;
}

inline void handle_keyboard() {
    // Read the scancode directly from the PS/2 data port — same port
    // keyboard.h's polling code reads, just now we're reading it because
    // an interrupt told us a byte is ready, instead of spinning to check.
    constexpr uint16_t PS2_DATA_PORT = 0x60;
    uint8_t scancode;
    asm volatile("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)PS2_DATA_PORT));

    if (g_keyboard_callback != nullptr) {
        g_keyboard_callback(scancode);
    }
}

} // namespace irq

extern "C" void irq_common_handler(interrupts::InterruptFrame* frame) {
    uint64_t vector = frame->vector_number;

    // Vector 0x80 is the software "yield" interrupt (see tasking.h) - it
    // was never raised by the PIC, so it must NOT be EOI'd: sending an
    // EOI for an interrupt the PIC doesn't think is in service confuses
    // its internal state and can cause real hardware IRQs to stop being
    // delivered correctly. Handle it as a complete no-op here; the actual
    // task-switching decision happens separately in tasking_maybe_switch,
    // called directly from the assembly stub after this function returns.
    if (vector == 0x80) {
        return;
    }

    uint8_t irq_line = (uint8_t)(vector - pic::IRQ_BASE);

    switch (irq_line) {
        case 0: irq::handle_timer();    break;
        case 1: irq::handle_keyboard(); break;
        default: break; // shouldn't happen — only IRQ0/1 are unmasked/wired
    }

    pic::send_eoi(irq_line);
}
