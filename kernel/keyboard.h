// keyboard.h — minimal polling PS/2 keyboard driver for bare-metal x86_64.
//
// No interrupts are used (no IDT set up yet in this project), so reading
// the keyboard means actively polling the controller's status port in a
// loop until a key event shows up. That's why read_line() below "blocks" —
// it's spinning on inb() until something arrives.

#pragma once
#include <stdint.h>

namespace kbd {

// ---------------------------------------------------------------------------
// Raw port I/O — the CPU instructions for talking to hardware ports directly
// (as opposed to memory-mapped I/O, which is just normal pointer access).
// ---------------------------------------------------------------------------
inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// ---------------------------------------------------------------------------
// PS/2 controller ports
// ---------------------------------------------------------------------------
constexpr uint16_t DATA_PORT = 0x60;
constexpr uint16_t STATUS_PORT = 0x64;
constexpr uint8_t OUTPUT_FULL = 0x01; // bit 0 of status: 1 = byte waiting to be read

// ---------------------------------------------------------------------------
// US QWERTY scancode set 1 -> ASCII, unshifted and shifted.
// Index = scancode (the "make code" sent when a key goes down).
// 0 means "no printable ASCII for this scancode" (e.g. modifier keys, F-keys).
// Only scancodes < 0x60 are populated; that covers the main alphanumeric block.
// ---------------------------------------------------------------------------
constexpr char SCANCODE_TO_ASCII[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,  '*', 0, ' ',
    // rest are F-keys, numpad, etc — left as 0 (unhandled) for this minimal driver
};

constexpr char SCANCODE_TO_ASCII_SHIFTED[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',
    0,  '*', 0, ' ',
};

constexpr uint8_t SCANCODE_LSHIFT = 0x2A;
constexpr uint8_t SCANCODE_RSHIFT = 0x36;
constexpr uint8_t SCANCODE_RELEASE_BIT = 0x80; // set in the byte when a key is released

// ---------------------------------------------------------------------------
// Driver state + functions
// ---------------------------------------------------------------------------
class Keyboard {
public:
    Keyboard() : shift_held(false) {}

    // Blocks until a key is pressed, returns its ASCII value.
    // Returns 0 for keys with no ASCII mapping (arrows, F-keys, etc) —
    // callers should just loop again in that case.
    char poll_char() {
        while (true) {
            uint8_t scancode = wait_for_scancode();

            bool released = scancode & SCANCODE_RELEASE_BIT;
            uint8_t code = scancode & ~SCANCODE_RELEASE_BIT;

            if (code == SCANCODE_LSHIFT || code == SCANCODE_RSHIFT) {
                shift_held = !released;
                continue; // shift itself isn't a printable character
            }

            if (released) {
                continue; // we only care about key-down events for typing
            }

            if (code >= 128) {
                continue;
            }

            char c = shift_held ? SCANCODE_TO_ASCII_SHIFTED[code] : SCANCODE_TO_ASCII[code];
            if (c != 0) {
                return c;
            }
            // unmapped key (F-keys etc) — ignore and keep polling
        }
    }

private:
    uint8_t wait_for_scancode() {
        while (!(inb(STATUS_PORT) & OUTPUT_FULL)) {
            // spin — nothing waiting yet
        }
        return inb(DATA_PORT);
    }

    bool shift_held;
};

} // namespace kbd
