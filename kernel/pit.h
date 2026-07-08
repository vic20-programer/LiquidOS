// pit.h — driver for the 8253/8254 Programmable Interval Timer (PIT).
//
// The PIT is ancient (it's been in PCs since the original IBM PC, and is
// still faithfully emulated today for backward compatibility) but it's
// the simplest way to get a regular timer interrupt: program a divisor,
// and it fires IRQ0 at a steady rate forever.
//
// The PIT's input clock runs at a fixed, oddly-specific frequency:
// 1193182 Hz (a historical artifact of how it was originally derived
// from the NTSC color clock on the original IBM PC — not something we
// get to choose). To get an interrupt every N Hz, you program the PIT
// with a divisor of (1193182 / N), and it counts down from that divisor
// at its fixed clock rate, firing IRQ0 each time it reaches zero.

#pragma once
#include <stdint.h>

namespace pit {

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

constexpr uint32_t PIT_BASE_FREQUENCY = 1193182; // Hz, fixed by the hardware

constexpr uint16_t PIT_CHANNEL0_DATA = 0x40;
constexpr uint16_t PIT_COMMAND = 0x43;

// Configures the PIT to fire IRQ0 at approximately the given frequency.
// "Approximately" because the divisor is an integer, so unless
// frequency_hz divides 1193182 evenly, there's a small rounding error —
// completely negligible for something like a 100 Hz scheduler/blink tick.
inline void set_frequency(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;

    // Command byte: channel 0, lobyte/hibyte access mode, mode 3
    // (square wave generator — the standard choice for a periodic tick)
    outb(PIT_COMMAND, 0x36);

    // Divisor is sent as two separate bytes, low byte first
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
}

} // namespace pit
