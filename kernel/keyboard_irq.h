// keyboard_irq.h — IRQ-driven keyboard input with arrow key support.
//
// IRQ1 fires the instant a key event happens (see irq.h's handle_keyboard,
// which reads the raw scancode byte and forwards it here via a callback).
// We push raw scancode BYTES into a ring buffer; arrow keys and other
// "extended" keys are multi-byte sequences (0xE0 prefix + a code byte),
// so the decoding logic below has to track that prefix across two
// separate on_scancode() calls before it knows what key was pressed.
//
// WHY A NEW "Key" TYPE INSTEAD OF JUST char: arrow keys have no ASCII
// representation — there's no character that means "move left." Cramming
// them into the `char` return type would mean picking some unused byte
// value to mean "left arrow," which is fragile and easy to confuse with
// real input later. Instead, read_key() returns a small tagged struct:
// either "here's a printable character" or "here's a named special key."

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace kbd_irq {

constexpr char SCANCODE_TO_ASCII[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,  '*', 0, ' ',
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
constexpr uint8_t SCANCODE_RELEASE_BIT = 0x80;
constexpr uint8_t SCANCODE_EXTENDED_PREFIX = 0xE0;

// Scancode set 1 extended ("0xE0-prefixed") codes for the arrow keys —
// these specific byte values are an architectural fact of PS/2 hardware,
// not something we get to choose.
constexpr uint8_t SCANCODE_EXT_LEFT  = 0x4B;
constexpr uint8_t SCANCODE_EXT_RIGHT = 0x4D;
constexpr uint8_t SCANCODE_EXT_UP    = 0x48;
constexpr uint8_t SCANCODE_EXT_DOWN  = 0x50;

enum class KeyKind {
    CHAR,       // a regular printable/control character (use .ch)
    ARROW_LEFT,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
};

struct Key {
    KeyKind kind;
    char ch; // valid only when kind == CHAR
};

inline Key make_char_key(char c) {
    Key k;
    k.kind = KeyKind::CHAR;
    k.ch = c;
    return k;
}

inline Key make_special_key(KeyKind kind) {
    Key k;
    k.kind = kind;
    k.ch = 0;
    return k;
}

constexpr size_t BUFFER_SIZE = 256;

class KeyboardIrqDriver {
public:
    KeyboardIrqDriver() : head(0), tail(0), shift_held(false), pending_extended(false) {}

    // Called from interrupt context (irq.h) — must stay fast and must
    // not allocate, block, or touch anything that itself waits on
    // interrupts (that would deadlock, since we're INSIDE one).
    void on_scancode(uint8_t scancode) {
        size_t next_head = (head + 1) % BUFFER_SIZE;
        if (next_head != tail) {
            buffer[head] = scancode;
            head = next_head;
        }
        // if full, silently drop — extremely unlikely at human typing speed
    }

    // Blocks (via `hlt`, not busy-spinning) until a full key event is
    // available, then returns it. on_idle is called on every wake-up with
    // no event ready yet (see cursor blink usage in kernel.cpp).
    template <typename IdleFn>
    Key read_key(IdleFn on_idle) {
        while (true) {
            uint8_t scancode;
            if (!try_pop(scancode)) {
                asm volatile("hlt");
                on_idle();
                continue;
            }

            // Extended-key prefix: the NEXT byte is the actual code for
            // an arrow key, Insert, Delete, Home, End, etc. We only
            // decode arrow keys for now; any other 0xE0-prefixed code
            // falls through to "unmapped, keep waiting" below.
            if (scancode == SCANCODE_EXTENDED_PREFIX) {
                pending_extended = true;
                continue;
            }

            bool released = scancode & SCANCODE_RELEASE_BIT;
            uint8_t code = scancode & ~SCANCODE_RELEASE_BIT;

            if (pending_extended) {
                pending_extended = false;
                if (released) {
                    continue; // we only act on key-down, same as regular keys
                }
                switch (code) {
                    case SCANCODE_EXT_LEFT:  return make_special_key(KeyKind::ARROW_LEFT);
                    case SCANCODE_EXT_RIGHT: return make_special_key(KeyKind::ARROW_RIGHT);
                    case SCANCODE_EXT_UP:    return make_special_key(KeyKind::ARROW_UP);
                    case SCANCODE_EXT_DOWN:  return make_special_key(KeyKind::ARROW_DOWN);
                    default: continue; // unmapped extended key - ignore
                }
            }

            if (code == SCANCODE_LSHIFT || code == SCANCODE_RSHIFT) {
                shift_held = !released;
                continue;
            }

            if (released || code >= 128) {
                continue;
            }

            char c = shift_held ? SCANCODE_TO_ASCII_SHIFTED[code] : SCANCODE_TO_ASCII[code];
            if (c != 0) {
                return make_char_key(c);
            }
            // unmapped key - keep waiting
        }
    }

    Key read_key() {
        return read_key([]() {});
    }

private:
    bool try_pop(uint8_t& out) {
        if (head == tail) return false;
        out = buffer[tail];
        tail = (tail + 1) % BUFFER_SIZE;
        return true;
    }

    uint8_t buffer[BUFFER_SIZE];
    size_t head, tail;
    bool shift_held;
    bool pending_extended; // true if we just saw 0xE0 and are awaiting the next byte
};

} // namespace kbd_irq
