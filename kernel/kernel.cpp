// kernel.cpp — LiquidOS kernel, NIC detection milestone.
//
// Starts a new series: networking, motivated directly by the USB series'
// outcome (see ../liquidos-usbfs/README.md's "Known limitation" section)
// - real-hardware USB mass storage on the ProBook 6450b hit an
// unresolved PCI Master Abort after 13 rounds of verified diagnosis, so
// getting files onto that machine's LiquidFS needs a different path.
// Long-term goal: enough of a network stack (NIC driver, ARP, IP, TCP,
// DNS, HTTP) to actually download a webpage on real hardware - but the
// immediate, practical target is much smaller: raw Ethernet frame TX/RX
// plus a trivial custom transfer protocol would already solve the
// original file-transfer problem, well before DNS/TCP/HTTP exist.
//
// This milestone only finds and classifies whatever Ethernet controller
// is present via PCI - mirrors the USB series' very first milestone
// (PCI enumeration) exactly in scope. Unlike USB host controllers
// (a handful of standard prog-if-identified register interfaces), real
// NIC hardware has no such standardization - Realtek, Broadcom, and
// Intel chips each have their own register layout, identified by
// vendor_id/device_id rather than class-level prog-if. A later milestone
// can't pick a driver strategy until this identifies the exact chip on
// the real target hardware - `lspci` now flags any Ethernet controller
// found, printing its vendor/device ID for exactly that reason.

#include <stdint.h>
#include <stddef.h>
#include "allocator.h"
#include "heap.h"
#include "tasking.h"
#include "idt_init.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "irq.h"
#include "keyboard_irq.h"
#include "cursor.h"
#include "ata.h"
#include "fs.h"
#include "pci.h"
#include "mmio.h"
#include "ehci.h"
#include "usb.h"
#include "msc.h"
#include "nic.h"

namespace vga {

constexpr uint16_t WIDTH = 80;
constexpr uint16_t HEIGHT = 25;
volatile uint16_t* const BUFFER = (volatile uint16_t*)0xb8000;

enum class Color : uint8_t {
    Black = 0, Blue = 1, Green = 2, Cyan = 3, Red = 4,
    Magenta = 5, Brown = 6, LightGrey = 7, DarkGrey = 8,
    LightBlue = 9, LightGreen = 10, LightCyan = 11, LightRed = 12,
    LightMagenta = 13, Yellow = 14, White = 15
};

// ---------------------------------------------------------------------------
// Terminal: the cursor overlay is now ALWAYS un-drawn before any operation
// that changes what's on screen at the cursor's position, and ALWAYS
// re-drawn after. The public write_char()/write() do this automatically,
// so callers never have to remember to call set_cursor_visible() around
// every individual operation — that manual bookkeeping is exactly what
// caused the original bug. cursor_currently_drawn tracks whether the
// overlay is presently painted, so un-draw is always a no-op-safe call
// even if it's invoked when the cursor wasn't visible to begin with.
// ---------------------------------------------------------------------------
class Terminal {
public:
    Terminal()
        : row(0), col(0), color(make_color(Color::LightGrey, Color::Black)),
          cursor_currently_drawn(false) {
        clear();
    }

    void clear() {
        for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
            BUFFER[i] = entry(' ', color);
        }
        row = 0;
        col = 0;
        cursor_currently_drawn = false;
    }

    void set_color(Color fg, Color bg) {
        color = make_color(fg, bg);
    }

    // Writes a character and advances the cursor, exactly like before.
    // Un-draws the cursor overlay first (if it was drawn) so the write
    // always lands on the TRUE cell content, never on an inverted overlay
    // byte — this is what backspace was missing.
    void write_char(char c) {
        undraw_cursor_if_needed();

        if (c == '\n') {
            newline();
        } else if (c == '\b') {
            backspace();
        } else {
            BUFFER[row * WIDTH + col] = entry(c, color);
            if (++col >= WIDTH) {
                newline();
            }
        }
    }

    void write(const char* str) {
        while (*str) {
            write_char(*str++);
        }
    }

    size_t cursor_row() const { return row; }
    size_t cursor_col() const { return col; }

    // Moves the cursor to an arbitrary column on the CURRENT row only —
    // used by arrow-key handling in the line editor. Deliberately doesn't
    // support moving rows; this kernel's line editor is single-line, so
    // there's no need (yet) for the cursor to leave the current row
    // during editing.
    void move_cursor_to_col(size_t new_col) {
        undraw_cursor_if_needed();
        if (new_col > WIDTH - 1) new_col = WIDTH - 1;
        col = new_col;
    }

    // Redraws whatever character is actually at column `c` on the current
    // row, using the terminal's current color — used when the line editor
    // needs to repaint part of the line after an insert/delete shifts
    // characters around. Does NOT move the logical cursor position.
    void put_char_at(size_t c, char ch) {
        BUFFER[row * WIDTH + c] = entry(ch, color);
    }

    // Sets cursor blink visibility. This is the ONLY place that paints
    // the inverted overlay — write_char/move_cursor_to_col always
    // un-draw first, so by the time this is called with `visible=true`,
    // the cell underneath is guaranteed to be the true, current content.
    void set_cursor_visible(bool visible) {
        if (visible) {
            draw_cursor();
        } else {
            undraw_cursor_if_needed();
        }
    }

private:
    static uint16_t make_color(Color fg, Color bg) {
        return (static_cast<uint8_t>(bg) << 4) | static_cast<uint8_t>(fg);
    }

    static uint16_t entry(char c, uint16_t color) {
        return (uint16_t)c | (color << 8);
    }

    void draw_cursor() {
        if (cursor_currently_drawn) return; // already drawn, nothing to do
        uint16_t existing = BUFFER[row * WIDTH + col];
        uint8_t existing_color = (existing >> 8) & 0xFF;
        uint8_t fg = existing_color & 0x0F;
        uint8_t bg = (existing_color >> 4) & 0x0F;
        uint8_t inverted = (uint8_t)((fg << 4) | bg);
        char ch = (char)(existing & 0xFF);
        BUFFER[row * WIDTH + col] = (uint16_t)ch | (uint16_t)(inverted << 8);
        cursor_currently_drawn = true;
    }

    // Un-draws the cursor at wherever row/col CURRENTLY point — must be
    // called BEFORE row/col change, never after, or it'll restore the
    // wrong cell (this was the root cause of the original bug).
    void undraw_cursor_if_needed() {
        if (!cursor_currently_drawn) return;
        uint16_t existing = BUFFER[row * WIDTH + col];
        uint8_t existing_color = (existing >> 8) & 0xFF;
        uint8_t fg = existing_color & 0x0F;
        uint8_t bg = (existing_color >> 4) & 0x0F;
        uint8_t un_inverted = (uint8_t)((fg << 4) | bg); // inverting twice = original
        char ch = (char)(existing & 0xFF);
        BUFFER[row * WIDTH + col] = (uint16_t)ch | (uint16_t)(un_inverted << 8);
        cursor_currently_drawn = false;
    }

    void newline() {
        col = 0;
        if (++row >= HEIGHT) {
            scroll();
            row = HEIGHT - 1;
        }
    }

    void backspace() {
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = WIDTH - 1;
        }
        BUFFER[row * WIDTH + col] = entry(' ', color);
    }

    void scroll() {
        for (size_t r = 1; r < HEIGHT; r++) {
            for (size_t c = 0; c < WIDTH; c++) {
                BUFFER[(r - 1) * WIDTH + c] = BUFFER[r * WIDTH + c];
            }
        }
        for (size_t c = 0; c < WIDTH; c++) {
            BUFFER[(HEIGHT - 1) * WIDTH + c] = entry(' ', color);
        }
    }

    size_t row, col;
    uint16_t color;
    bool cursor_currently_drawn;
};

} // namespace vga

class VgaInterruptOutput : public interrupts::Output {
public:
    explicit VgaInterruptOutput(vga::Terminal& t) : term(t) {}

    void write(const char* s) override {
        term.set_color(vga::Color::LightRed, vga::Color::Black);
        term.write(s);
    }

    void write_hex(uint64_t value) override {
        term.set_color(vga::Color::Yellow, vga::Color::Black);
        term.write("0x");
        char buf[17];
        const char* digits = "0123456789abcdef";
        for (int i = 15; i >= 0; i--) {
            buf[15 - i] = digits[(value >> (i * 4)) & 0xF];
        }
        buf[16] = '\0';
        term.write(buf);
    }

private:
    vga::Terminal& term;
};

static kbd_irq::KeyboardIrqDriver* g_keyboard_driver = nullptr;

static void keyboard_irq_trampoline(uint8_t scancode) {
    if (g_keyboard_driver != nullptr) {
        g_keyboard_driver->on_scancode(scancode);
    }
}

// ---------------------------------------------------------------------------
// Editable line buffer + cursor movement.
//
// MODEL: `buf` holds the line's characters, `len` is how many are
// currently in use, `edit_pos` is where the next typed character would
// be INSERTED (0 <= edit_pos <= len). This is deliberately separate from
// the terminal's on-screen column — line_start_col remembers where the
// line began on screen, so screen_col = line_start_col + edit_pos always.
//
// Typing inserts at edit_pos (shifting everything after it right) and
// backspace deletes the character BEFORE edit_pos (shifting everything
// after it left) — both need the whole tail of the line redrawn, since
// every character from the edit point onward shifts on screen.
// ---------------------------------------------------------------------------
struct EditLine {
    char* buf;
    size_t len;
    size_t edit_pos;
    size_t max_len;
    size_t line_start_col;

    void reset(size_t start_col) {
        len = 0;
        edit_pos = 0;
        line_start_col = start_col;
    }
};

// Redraws buf[from_index..len) on screen, used after an insert or delete
// shifts the tail of the line. `from_index` is passed explicitly rather
// than always using line.edit_pos, because by the time this is called
// edit_pos has typically already moved past the character that was just
// inserted/deleted — the caller knows exactly which index needs repainting.
// Leaves one extra blank cell at the end when shrinking (delete) so the
// previously-last character doesn't leave a visual trail behind.
static void redraw_tail(vga::Terminal& term, EditLine& line, size_t from_index, bool shrunk) {
    size_t screen_col = line.line_start_col + from_index;
    for (size_t i = from_index; i < line.len; i++) {
        term.put_char_at(screen_col++, line.buf[i]);
    }
    if (shrunk && screen_col < vga::WIDTH) {
        term.put_char_at(screen_col, ' ');
    }
}

static size_t read_line(vga::Terminal& term, kbd_irq::KeyboardIrqDriver& keyboard,
                         cursor::BlinkState& blink, char* buf, size_t max_len) {
    EditLine line;
    line.buf = buf;
    line.max_len = max_len;
    line.reset(term.cursor_col());

    term.set_cursor_visible(true);

    auto on_idle = [&]() {
        if (blink.update(irq::g_tick_count)) {
            term.set_cursor_visible(blink.is_visible());
        }
    };

    auto move_cursor_to_edit_pos = [&]() {
        term.move_cursor_to_col(line.line_start_col + line.edit_pos);
    };

    while (true) {
        kbd_irq::Key key = keyboard.read_key(on_idle);

        term.set_cursor_visible(false);
        blink.reset_to_visible(irq::g_tick_count);

        if (key.kind == kbd_irq::KeyKind::ARROW_LEFT) {
            if (line.edit_pos > 0) {
                line.edit_pos--;
                move_cursor_to_edit_pos();
            }
            term.set_cursor_visible(true);
            continue;
        }

        if (key.kind == kbd_irq::KeyKind::ARROW_RIGHT) {
            if (line.edit_pos < line.len) {
                line.edit_pos++;
                move_cursor_to_edit_pos();
            }
            term.set_cursor_visible(true);
            continue;
        }

        if (key.kind == kbd_irq::KeyKind::ARROW_UP || key.kind == kbd_irq::KeyKind::ARROW_DOWN) {
            // No command history yet - nothing to do, but consume the
            // key rather than falling through to character handling.
            term.set_cursor_visible(true);
            continue;
        }

        // From here on, key.kind == CHAR.
        char c = key.ch;

        if (c == '\n') {
            // Move the terminal's real cursor to the end of the line
            // before printing the newline, regardless of where the edit
            // cursor was — otherwise pressing Enter mid-line would leave
            // the visible cursor (and the next prompt) at the wrong column.
            term.move_cursor_to_col(line.line_start_col + line.len);
            term.write_char('\n');
            buf[line.len] = '\0';
            return line.len;
        }

        if (c == '\b') {
            if (line.edit_pos > 0) {
                // Shift everything after the deletion point one slot left...
                for (size_t i = line.edit_pos - 1; i < line.len - 1; i++) {
                    line.buf[i] = line.buf[i + 1];
                }
                line.len--;
                line.edit_pos--;
                // ...then redraw from the new edit_pos onward, including
                // clearing the now-stale last character.
                move_cursor_to_edit_pos();
                redraw_tail(term, line, line.edit_pos, /*shrunk=*/true);
                move_cursor_to_edit_pos();
            }
            // edit_pos == 0: nothing before the cursor to delete, no-op —
            // this is also what stops backspace from ever moving the
            // cursor onto the previous screen row in this milestone.
        } else if (line.len < line.max_len) {
            // Insert at edit_pos: shift everything from edit_pos onward
            // right by one to make room, then place the new character.
            size_t insert_index = line.edit_pos;
            for (size_t i = line.len; i > insert_index; i--) {
                line.buf[i] = line.buf[i - 1];
            }
            line.buf[insert_index] = c;
            line.len++;
            line.edit_pos++;
            redraw_tail(term, line, insert_index, /*shrunk=*/false);
            move_cursor_to_edit_pos();
        }

        term.set_cursor_visible(true);
    }
}

static bool line_is(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        s++; prefix++;
    }
    return true;
}

// Backed by .bss, not the call stack — a function-local `static` would
// need a compiler-generated init guard (__cxa_guard_acquire/release),
// which this freestanding kernel has no runtime support for.
static char g_cat_buffer[4096];

__attribute__((noinline)) static void trigger_divide_by_zero() {
    volatile int64_t a = 10;
    volatile int64_t b = 0;
    volatile int64_t result = a / b;
    (void)result;
}

__attribute__((noinline)) static void trigger_page_fault() {
    volatile uint8_t* bad_ptr = (volatile uint8_t*)0xdeadbeef000ULL;
    volatile uint8_t value = *bad_ptr;
    (void)value;
}

static void print_uint(vga::Terminal& term, uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) {
        term.write("0");
        return;
    }
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    term.write(&buf[i]);
}

// Fixed-width, zero-padded hex - used for PCI vendor/device IDs (4
// digits, conventionally) and class/subclass/prog-if (2 digits each),
// matching how `lspci`-style tools traditionally print them.
static void print_hex(vga::Terminal& term, uint32_t v, int digits) {
    char buf[9];
    const char* hex_digits = "0123456789abcdef";
    for (int i = 0; i < digits; i++) {
        buf[digits - 1 - i] = hex_digits[(v >> (i * 4)) & 0xF];
    }
    buf[digits] = '\0';
    term.write(buf);
}

// Shared by `lspci`'s unfiltered and filtered (`lspci net`/`lspci usb`)
// forms. Split out because a real 6450b has enough PCI devices that the
// unfiltered list scrolls past the 25-line VGA screen before the entry
// you actually want (the Ethernet controller) can be read - the filtered
// forms reuse this to print just the matching device(s).
static void print_pci_device(vga::Terminal& term, const pci::DeviceInfo& d) {
    term.write("  ");
    print_uint(term, d.bus);
    term.write(":");
    print_uint(term, d.device);
    term.write(".");
    print_uint(term, d.function);
    term.write("  vendor=");
    print_hex(term, d.vendor_id, 4);
    term.write(" device=");
    print_hex(term, d.device_id, 4);
    term.write("  class=");
    print_hex(term, d.class_code, 2);
    term.write(":");
    print_hex(term, d.subclass, 2);
    term.write(":");
    print_hex(term, d.prog_if, 2);

    if (d.class_code == pci::CLASS_SERIAL_BUS && d.subclass == pci::SUBCLASS_USB) {
        term.set_color(vga::Color::Yellow, vga::Color::Black);
        term.write("  <- USB host controller (");
        if (d.prog_if == pci::PROGIF_UHCI) term.write("UHCI");
        else if (d.prog_if == pci::PROGIF_OHCI) term.write("OHCI");
        else if (d.prog_if == pci::PROGIF_EHCI) term.write("EHCI");
        else if (d.prog_if == pci::PROGIF_XHCI) term.write("xHCI");
        else term.write("unknown type");
        term.write(", BAR0=");
        print_hex(term, d.bar[0], 8);
        term.write(")");
        term.set_color(vga::Color::LightCyan, vga::Color::Black);
    }

    if (d.class_code == pci::CLASS_NETWORK && d.subclass == pci::SUBCLASS_ETHERNET) {
        term.set_color(vga::Color::Yellow, vga::Color::Black);
        term.write("  <- Ethernet controller (vendor=");
        print_hex(term, d.vendor_id, 4);
        term.write(" device=");
        print_hex(term, d.device_id, 4);
        term.write(", BAR0=");
        print_hex(term, d.bar[0], 8);
        term.write(")");
        term.set_color(vga::Color::LightCyan, vga::Color::Black);
    }
    term.write("\n");
}

// Shared by `usbenum` and `usbmsc`: prints WHICH step of usb::enumerate()
// failed and WHY, using the diagnostic-only state control_transfer()/
// enumerate() leave behind (see ehci.h/usb.h) - added specifically because
// a real-hardware test reported "Enumeration failed" twice with no way to
// tell whether it was a timeout (queue head never advanced - a driver bug)
// or a real USB error response (STALL/babble/etc - a protocol/compat
// issue), and re-running a real-hardware test costs a full reboot, so this
// diagnostic needs to be as complete as possible on the very next attempt.
// Re-reads the USB Legacy Support capability RIGHT NOW (not just what
// bios_handoff() observed during its own timeout window) - if
// USBLEGSUP_BIOS_OWNED is set again by the time a transfer has failed,
// that's direct evidence the BIOS's own SMM code (which we already know
// services the keyboard through this exact controller) is still actively
// reclaiming/touching it after our handoff, not just something that
// happened once at boot. This chipset (Intel 5 Series/Ibex Peak PCH,
// 8086:3b34/8086:3b3c) is extremely common, well-documented silicon, which
// argues against an obscure chipset erratum and toward something more
// systemic like this being the real explanation instead.
static void print_legsup_status(vga::Terminal& term, const pci::DeviceInfo& dev) {
    term.write("  [diag] BIOS handoff outcome: ");
    switch (ehci::g_last_handoff_result) {
        case ehci::HandoffResult::NO_CAPABILITY:   term.write("no Legacy Support capability found"); break;
        case ehci::HandoffResult::ALREADY_OS_OWNED: term.write("was already OS-owned"); break;
        case ehci::HandoffResult::BIOS_RELEASED:    term.write("BIOS released it"); break;
        case ehci::HandoffResult::BIOS_TIMED_OUT:   term.write("BIOS never released it (timed out)"); break;
    }
    if (ehci::g_last_legsup_cap_offset != 0) {
        uint32_t now = pci::read_config_dword(dev.bus, dev.device, dev.function, ehci::g_last_legsup_cap_offset);
        term.write("\n  [diag] Legacy Support capability RIGHT NOW: 0x");
        print_hex(term, now, 8);
        term.write(" (BIOS_OWNED=");
        term.write((now & ehci::USBLEGSUP_BIOS_OWNED) ? "SET <- BIOS has reclaimed it" : "clear");
        term.write(" OS_OWNED=");
        term.write((now & ehci::USBLEGSUP_OS_OWNED) ? "SET" : "clear");
        term.write(")\n");
    } else {
        term.write("\n");
    }
}

static void print_enum_failure(vga::Terminal& term, ehci::Controller& controller, const pci::DeviceInfo& dev) {
    term.set_color(vga::Color::LightRed, vga::Color::Black);
    term.write("Enumeration failed (a control transfer did not complete).\n");
    term.write("  Step: ");
    term.write(usb::g_last_enum_step ? usb::g_last_enum_step : "(unknown)");
    term.write("\n  Last observed qTD token: 0x");
    print_hex(term, ehci::g_last_transfer_token, 8);
    term.write("\n  ");
    if (ehci::g_last_transfer_timed_out) {
        term.write("Timed out - Active bit never cleared within the timeout window.\n");
    } else {
        term.write("Error bit(s) set:");
        uint32_t t = ehci::g_last_transfer_token;
        if (t & ehci::QTD_STATUS_HALTED) term.write(" Halted(STALL/unrecoverable)");
        if (t & ehci::QTD_STATUS_BUFFER_ERROR) term.write(" BufferError");
        if (t & ehci::QTD_STATUS_BABBLE) term.write(" Babble");
        if (t & ehci::QTD_STATUS_TRANSACTION_ERROR) term.write(" TransactionError(3 retries failed)");
        term.write("\n");
    }

    // Extra diagnostics added after two blind fix attempts (Bus Master
    // Enable, CTRLDSSEGMENT) both failed to change this exact symptom on
    // real hardware - rather than guess a third time, gather hard
    // evidence: does this controller even claim 64-bit addressing
    // support (tells us whether the CTRLDSSEGMENT fix could possibly have
    // mattered), is USBSTS's Host System Error bit set (confirms/rules
    // out a genuine PCI/DMA-level fault directly, never checked before),
    // and is FRINDEX advancing at all (confirms/rules out the schedule
    // engine running at all, independent of DMA correctness).
    uint32_t hccparams = mmio::read32(controller.bar_base + ehci::HCCPARAMS_OFFSET);
    uint32_t usbsts = mmio::read32(controller.op_base + ehci::USBSTS_OFFSET);
    uint32_t frindex_before = mmio::read32(controller.op_base + ehci::FRINDEX_OFFSET);
    for (volatile int i = 0; i < 2000000; i++) ehci::cpu_pause(); // brief spin, just to sample FRINDEX twice
    uint32_t frindex_after = mmio::read32(controller.op_base + ehci::FRINDEX_OFFSET);
    // Directly verifying an assumption rather than guessing again: the
    // Master Abort followed these structures to a completely different
    // physical address 8MiB away (see the address-range experiment),
    // which rules out anything specific to the original low address and
    // reopens whether CTRLDSSEGMENT (written once, during reset, never
    // actually read back to confirm) truly holds 0 at the moment of
    // failure - if it doesn't, every address this driver ever hands the
    // controller gets paired with the same garbage high 32 bits, which
    // would explain a Master Abort following the structures to ANY low-
    // 32-bit address unchanged.
    uint32_t ctrldssegment = mmio::read32(controller.op_base + ehci::CTRLDSSEGMENT_OFFSET);

    term.write("  CTRLDSSEGMENT=0x");
    print_hex(term, ctrldssegment, 8);
    term.write((ctrldssegment == 0) ? " (confirmed zero)\n" : " (NOT ZERO - this is the bug)\n");

    term.write("  HCCPARAMS=0x");
    print_hex(term, hccparams, 8);
    term.write(" (64-bit addressing: ");
    term.write((hccparams & ehci::HCCPARAMS_64BIT_ADDRESSING) ? "yes" : "no");
    term.write(")\n  USBSTS=0x");
    print_hex(term, usbsts, 8);
    term.write(" (HostSystemError=");
    term.write((usbsts & ehci::USBSTS_HOST_SYSTEM_ERROR) ? "SET" : "clear");
    term.write(" USBErrorInt=");
    term.write((usbsts & ehci::USBSTS_USB_ERROR_INT) ? "SET" : "clear");
    term.write(" FrameListRollover=");
    term.write((usbsts & ehci::USBSTS_FRAME_LIST_ROLLOVER) ? "SET" : "clear");
    term.write(")\n  FRINDEX before/after a brief spin: 0x");
    print_hex(term, frindex_before, 4);
    term.write(" / 0x");
    print_hex(term, frindex_after, 4);
    term.write((frindex_before == frindex_after) ? "  <- NOT advancing\n" : "  <- advancing\n");

    // USBSTS's Host System Error only says SOME bus-level fault happened,
    // not which kind - the PCI Status register (a different, standalone
    // register in config space) distinguishes Master Abort/Target Abort/
    // Parity Error for the same event. Not checked in any of this bug's
    // prior real-hardware rounds.
    uint16_t pci_status = pci::read_status(dev.bus, dev.device, dev.function);
    term.write("  PCI Status=0x");
    print_hex(term, pci_status, 4);
    term.write(" (");
    bool any = false;
    if (pci_status & pci::STATUS_RECEIVED_MASTER_ABORT) { term.write("ReceivedMasterAbort "); any = true; }
    if (pci_status & pci::STATUS_RECEIVED_TARGET_ABORT) { term.write("ReceivedTargetAbort "); any = true; }
    if (pci_status & pci::STATUS_SIGNALED_TARGET_ABORT) { term.write("SignaledTargetAbort "); any = true; }
    if (pci_status & pci::STATUS_SIGNALED_SYSTEM_ERROR) { term.write("SignaledSystemError "); any = true; }
    if (pci_status & pci::STATUS_DETECTED_PARITY_ERROR) { term.write("DetectedParityError "); any = true; }
    if (!any) term.write("none of Master/Target Abort or Parity Error set");
    term.write(")\n");

    // Received Master Abort means SOME physical address in the transfer
    // was never claimed by anything on the bus - but g_control_qh's own
    // address is already proven to DMA-fetch cleanly (see
    // print_async_enable_status()). A real transfer touches several MORE
    // addresses the empty-QH case never does: the qTD array itself, and
    // whichever buffer a qTD points at. Print all of them so an obviously
    // wrong one (a wild/garbage value, or one far outside this kernel's
    // own small memory footprint) would stand out immediately.
    term.write("  Other structure addresses - qTDs: 0x");
    print_hex(term, ehci::phys32(&ehci::g_control_qtds[0]), 8);
    term.write("  data buffer: 0x");
    print_hex(term, ehci::phys32(ehci::g_control_data_buffer), 8);
    term.write("\n  setup buffer: 0x");
    print_hex(term, ehci::phys32(ehci::g_control_setup_buffer), 8);
    term.write("  periodic list: 0x");
    print_hex(term, ehci::phys32(ehci::g_periodic_frame_list), 8);
    term.write("\n");

    print_legsup_status(term, dev);

    term.set_color(vga::Color::LightCyan, vga::Color::Black);
}

// Called immediately after enable_async_schedule() succeeds, before
// usb::enumerate() ever calls control_transfer() - narrows down WHETHER a
// later Host System Error (see print_enum_failure()) already happened the
// instant the schedule started fetching our (still completely empty)
// queue head, versus only appearing once control_transfer() builds a real
// qTD chain. If USBSTS already shows trouble here, the problem is in the
// queue head/ASYNCLISTADDR itself, not anything qTD-chain-specific.
static void print_async_enable_status(vga::Terminal& term, ehci::Controller& controller, const pci::DeviceInfo& dev) {
    uint32_t usbsts = mmio::read32(controller.op_base + ehci::USBSTS_OFFSET);
    term.write("  [diag] Queue head physical address: 0x");
    print_hex(term, ehci::phys32(&ehci::g_control_qh), 8);
    term.write("\n  [diag] USBSTS right after enabling the async schedule: 0x");
    print_hex(term, usbsts, 8);
    term.write(" (HostSystemError=");
    term.write((usbsts & ehci::USBSTS_HOST_SYSTEM_ERROR) ? "SET" : "clear");
    term.write(" HCHalted=");
    term.write((usbsts & ehci::USBSTS_HCHALTED) ? "SET" : "clear");
    term.write(")\n");
    print_legsup_status(term, dev);
}

// A one-off I/O-port read, used only by `usbprobe` to peek at a UHCI/
// OHCI controller's registers once pci::parse_bar() has found its I/O
// BAR - not part of any driver yet, just enough to prove real hardware
// access works, the same role mmio.h plays for memory-mapped BARs.
static inline uint16_t io_inw(uint16_t port) {
    uint16_t value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

// ---------------------------------------------------------------------------
// Demo cooperative tasks. Each one runs a fixed number of iterations,
// printing its identity and iteration count, yielding after each print —
// this makes the interleaving directly visible on screen: if cooperative
// switching works, you'll see Task A and Task B's output alternate line
// by line, not one task's full output followed by the other's.
//
// g_demo_terminal is a global pointer (same pattern as interrupts::
// g_output) since task entry functions take no arguments — tasking.h's
// TaskEntryFn is `void()`, deliberately kept simple for this milestone.
// ---------------------------------------------------------------------------
static vga::Terminal* g_demo_terminal = nullptr;

static void demo_task_a() {
    for (int i = 1; i <= 5; i++) {
        g_demo_terminal->set_color(vga::Color::LightGreen, vga::Color::Black);
        g_demo_terminal->write("  [Task A] iteration ");
        print_uint(*g_demo_terminal, i);
        g_demo_terminal->write("\n");
        tasking::yield();
    }
    g_demo_terminal->set_color(vga::Color::DarkGrey, vga::Color::Black);
    g_demo_terminal->write("  [Task A] finished.\n");
}

static void demo_task_b() {
    for (int i = 1; i <= 5; i++) {
        g_demo_terminal->set_color(vga::Color::LightCyan, vga::Color::Black);
        g_demo_terminal->write("  [Task B] iteration ");
        print_uint(*g_demo_terminal, i);
        g_demo_terminal->write("\n");
        tasking::yield();
    }
    g_demo_terminal->set_color(vga::Color::DarkGrey, vga::Color::Black);
    g_demo_terminal->write("  [Task B] finished.\n");
}

static void run_task_test(vga::Terminal& term) {
    g_demo_terminal = &term;

    term.set_color(vga::Color::Yellow, vga::Color::Black);
    term.write("Starting two cooperative tasks (A and B)...\n");
    term.write("If switching works, their output should ALTERNATE line by line.\n\n");

    bool ok_a = tasking::make_task(demo_task_a);
    bool ok_b = tasking::make_task(demo_task_b);

    if (!ok_a || !ok_b) {
        term.set_color(vga::Color::LightRed, vga::Color::Black);
        term.write("Failed to create one or both tasks!\n");
        return;
    }

    // Drive the round-robin by yielding from the "main" task (kmain's own
    // flow, registered as task slot 0) repeatedly. Each yield hands
    // control to whichever task is next in line - A, then B, then back
    // to here, then A again, and so on, until both finish.
    for (int i = 0; i < 12; i++) {
        tasking::yield();
    }

    term.set_color(vga::Color::Yellow, vga::Color::Black);
    term.write("\nTask test complete.\n");
}

// ---------------------------------------------------------------------------
// Preemption demo: TWO tasks, NEITHER of which EVER calls yield(). Each
// just spins in a tight loop checking the global tick counter, printing
// whenever a meaningful amount of time has passed. If preemption is
// genuinely working, you'll STILL see their output alternate — proving
// the timer interrupt is forcibly switching between them on its own
// schedule, with no cooperation from the task code at all. This is the
// one thing the previous (cooperative-only) milestone could never do:
// a task stuck in an infinite loop with no yield used to hang the whole
// kernel solid. Here, it doesn't.
// ---------------------------------------------------------------------------
static void spinning_task_a() {
    uint64_t last_print = irq::g_tick_count;
    int prints = 0;
    while (prints < 5) {
        // Deliberately a tight busy-loop with NO yield() call anywhere -
        // the only reason this task ever stops running is the timer
        // interrupt preempting it.
        if (irq::g_tick_count - last_print >= 30) { // ~0.3 sec at 100Hz
            last_print = irq::g_tick_count;
            prints++;
            g_demo_terminal->set_color(vga::Color::LightGreen, vga::Color::Black);
            g_demo_terminal->write("  [Spinner A] tick ");
            print_uint(*g_demo_terminal, irq::g_tick_count);
            g_demo_terminal->write(" (no yield() call - preempted by timer)\n");
        }
    }
    g_demo_terminal->set_color(vga::Color::DarkGrey, vga::Color::Black);
    g_demo_terminal->write("  [Spinner A] finished.\n");
}

static void spinning_task_b() {
    uint64_t last_print = irq::g_tick_count;
    int prints = 0;
    while (prints < 5) {
        if (irq::g_tick_count - last_print >= 30) {
            last_print = irq::g_tick_count;
            prints++;
            g_demo_terminal->set_color(vga::Color::LightCyan, vga::Color::Black);
            g_demo_terminal->write("  [Spinner B] tick ");
            print_uint(*g_demo_terminal, irq::g_tick_count);
            g_demo_terminal->write(" (no yield() call - preempted by timer)\n");
        }
    }
    g_demo_terminal->set_color(vga::Color::DarkGrey, vga::Color::Black);
    g_demo_terminal->write("  [Spinner B] finished.\n");
}

static void run_preempt_test(vga::Terminal& term) {
    g_demo_terminal = &term;

    term.set_color(vga::Color::Yellow, vga::Color::Black);
    term.write("Starting two SPINNING tasks - neither ever calls yield()!\n");
    term.write("If preemption works, their output should STILL alternate,\n");
    term.write("purely because the timer interrupt forces switches.\n\n");

    bool ok_a = tasking::make_task(spinning_task_a);
    bool ok_b = tasking::make_task(spinning_task_b);

    if (!ok_a || !ok_b) {
        term.set_color(vga::Color::LightRed, vga::Color::Black);
        term.write("Failed to create one or both tasks!\n");
        return;
    }

    // Unlike run_task_test(), the MAIN task itself also just busy-waits
    // here rather than cooperatively yielding - it's relying ENTIRELY on
    // timer preemption to ever let the spinner tasks run at all. If
    // preemption were broken, this would hang forever, since main never
    // yields and the spinners never yield either.
    uint64_t start_tick = irq::g_tick_count;
    while (irq::g_tick_count - start_tick < 400) { // ~4 seconds, generous timeout
        bool a_done = (tasking::g_tasks[1].state == tasking::TaskState::UNUSED);
        bool b_done = (tasking::g_tasks[2].state == tasking::TaskState::UNUSED);
        if (a_done && b_done) break;
    }

    term.set_color(vga::Color::Yellow, vga::Color::Black);
    term.write("\nPreemption test complete.\n");
}

// Allocates and frees a handful of differently-sized blocks live, in
// front of the user, then reports heap stats before/after/in-between —
// this is a runtime, on-hardware echo of the host-side test suite used
// to find and fix the original coalescing bug, so you can see the same
// behavior happening for real, on boot, on your machine.
static void run_heap_test(vga::Terminal& term) {
    term.set_color(vga::Color::LightCyan, vga::Color::Black);
    term.write("Heap test: allocating 3 blocks (64, 128, 32 bytes)...\n");

    void* a = heap2::alloc(64);
    void* b = heap2::alloc(128);
    void* c = heap2::alloc(32);

    term.write("  free blocks: ");
    print_uint(term, heap2::g_heap.count_free_blocks());
    term.write("   free bytes: ");
    print_uint(term, heap2::g_heap.total_free_bytes());
    term.write("\n");

    if (a == nullptr || b == nullptr || c == nullptr) {
        term.set_color(vga::Color::LightRed, vga::Color::Black);
        term.write("Allocation failed unexpectedly!\n");
        return;
    }

    term.write("Freeing the middle block (128 bytes)...\n");
    heap2::free_ptr(b);
    term.write("  free blocks: ");
    print_uint(term, heap2::g_heap.count_free_blocks());
    term.write("   free bytes: ");
    print_uint(term, heap2::g_heap.total_free_bytes());
    term.write("\n");

    term.write("Freeing the first block (64 bytes) - should coalesce with the middle...\n");
    heap2::free_ptr(a);
    term.write("  free blocks: ");
    print_uint(term, heap2::g_heap.count_free_blocks());
    term.write("   free bytes: ");
    print_uint(term, heap2::g_heap.total_free_bytes());
    term.write("\n");

    term.write("Freeing the last block (32 bytes) - should coalesce into one big block...\n");
    heap2::free_ptr(c);
    term.write("  free blocks: ");
    print_uint(term, heap2::g_heap.count_free_blocks());
    term.write("   free bytes: ");
    print_uint(term, heap2::g_heap.total_free_bytes());
    term.write("\n");

    term.set_color(vga::Color::LightGreen, vga::Color::Black);
    term.write("Heap test complete.\n");
}

extern "C" void kmain(void* multiboot_info_ptr) {
    (void)multiboot_info_ptr;

    vga::Terminal term;

    // Must happen before anything uses `new`/`delete` or heap2::alloc()
    // directly — there's no global constructor doing this automatically
    // (see allocator.h's note on why this kernel avoids relying on
    // C++ static initializers running before kmain()).
    heap2::init();
    tasking::init_main_task();

    VgaInterruptOutput interrupt_output(term);
    interrupts::g_output = &interrupt_output;

    idt::install_exception_handlers();
    pic::remap();
    pit::set_frequency(100);

    kbd_irq::KeyboardIrqDriver keyboard;
    g_keyboard_driver = &keyboard;
    irq::g_keyboard_callback = keyboard_irq_trampoline;

    pic::unmask_irq(0);
    pic::unmask_irq(1);
    // ata.h is polling-only (no IDT entry installed for vector 46), but
    // SeaBIOS leaves IRQ14 unmasked for its own legacy disk access, and
    // the ATA controller raises it regardless of whether anyone's
    // listening — explicitly mask it before any disk access or the
    // first PIO read delivers an unhandled interrupt straight into a #GP.
    pic::mask_irq(14);
    pic::mask_irq(15);

    asm volatile("sti");

    bool fs_ok = fs::mount_any();

    term.set_color(vga::Color::LightGreen, vga::Color::Black);
    term.write("LiquidOS kernel - NIC detection milestone\n");
    term.set_color(vga::Color::LightGrey, vga::Color::Black);
    term.write("Left/Right arrows move the cursor within the current line.\n");
    term.write("A free-list heap with malloc/free + coalescing now backs operator new.\n");
    if (fs_ok) {
        // Boot only ever auto-mounts an ATA drive (see fs::mount_any()'s
        // comment for why USB is deliberately excluded from this
        // automatic path) - fs::mounted_backend() is always ATA here.
        term.write("LiquidFS auto-mounted from drive ");
        print_uint(term, fs::mounted_drive());
        term.write(": ");
        print_uint(term, fs::file_count());
        term.write(" file(s).\n\n");
    } else {
        term.set_color(vga::Color::LightRed, vga::Color::Black);
        term.write("LiquidFS NOT mounted - no ATA disk with a valid superblock was found.\n");
        term.set_color(vga::Color::LightGrey, vga::Color::Black);
        term.write("Try 'lsdisk' to see what's attached, then 'mount <0-3>' or 'mount usb'.\n\n");
    }
    term.write("Commands:\n");
    term.write("  ticks     -> show how many timer ticks have elapsed since boot\n");
    term.write("  tasktest   -> run two cooperative tasks and watch their output interleave\n");
    term.write("  preempttest-> run two tasks that NEVER yield - proves the timer preempts them\n");
    term.write("  heaptest  -> allocate/free blocks live and show coalescing happen\n");
    term.write("  heapstats -> show current free block count and total free bytes\n");
    term.write("  ls        -> list files on the mounted LiquidFS disk\n");
    term.write("  cat <file>-> read a file off disk through the ATA PIO driver and print it\n");
    term.write("  write <file> <text> -> create/overwrite a file with the rest of the line\n");
    term.write("  rm <file> -> delete a file and return its sectors to the free list\n");
    term.write("  fsstat    -> show free-extent count and total free sectors on the mounted disk\n");
    term.write("  lsdisk    -> list all 4 addressable drives and which one is mounted\n");
    term.write("  mount <0-3> -> mount LiquidFS from that drive (0=primary master .. 3=secondary slave)\n");
    term.write("  mount usb -> find a USB mass-storage device and mount LiquidFS from it\n");
    term.write("  unmount   -> unmount the current filesystem\n");
    term.write("  lspci     -> list PCI devices, flagging any USB host or Ethernet controller found\n");
    term.write("  lspci net -> list ONLY Ethernet controllers (short output, won't scroll off-screen)\n");
    term.write("  lspci usb -> list ONLY USB host controllers (short output, won't scroll off-screen)\n");
    term.write("  usbprobe  -> parse/size the USB controller's BAR(s) and read a real register\n");
    term.write("  nicprobe  -> parse the Ethernet controller's BAR and read CTRL/STATUS\n");
    term.write("  usbup     -> bring an EHCI controller up and reset any port with a device on it\n");
    term.write("  usbenum   -> bring up EHCI, reset the first connected port, and enumerate the device\n");
    term.write("  usbmsc    -> enumerate a mass-storage device and read/write it via real SCSI commands\n");
    term.write("  de        -> trigger divide-by-zero, to confirm exceptions still work\n");
    term.write("  pf        -> trigger page fault, to confirm exceptions still work\n");
    term.write("  help      -> show this list again\n\n");

    cursor::BlinkState blink;
    char line[128];

    while (true) {
        term.set_color(vga::Color::White, vga::Color::Black);
        term.write("> ");

        read_line(term, keyboard, blink, line, sizeof(line) - 1);

        if (line_is(line, "ticks")) {
            term.set_color(vga::Color::LightCyan, vga::Color::Black);
            term.write("Ticks since boot: ");
            print_uint(term, irq::g_tick_count);
            term.write(" (at 100 Hz, that's ~");
            print_uint(term, irq::g_tick_count / 100);
            term.write(" sec)\n");
        } else if (line_is(line, "tasktest")) {
            run_task_test(term);
        } else if (line_is(line, "preempttest")) {
            run_preempt_test(term);
        } else if (line_is(line, "heaptest")) {
            run_heap_test(term);
        } else if (line_is(line, "heapstats")) {
            term.set_color(vga::Color::LightCyan, vga::Color::Black);
            term.write("Free blocks: ");
            print_uint(term, heap2::g_heap.count_free_blocks());
            term.write("   Free bytes: ");
            print_uint(term, heap2::g_heap.total_free_bytes());
            term.write(" / ");
            print_uint(term, heap2::POOL_SIZE);
            term.write("\n");
        } else if (line_is(line, "ls")) {
            term.set_color(vga::Color::LightCyan, vga::Color::Black);
            if (!fs::g_mounted) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No filesystem mounted.\n");
            } else {
                term.write("Files (");
                print_uint(term, fs::file_count());
                term.write("):\n");
                for (uint32_t i = 0; i < fs::file_count(); i++) {
                    const fs::DirEntry* e = fs::entry_at(i);
                    term.write("  ");
                    term.write(e->name);
                    term.write("  (");
                    print_uint(term, e->size_bytes);
                    term.write(" bytes)\n");
                }
            }
        } else if (starts_with(line, "cat ")) {
            const char* filename = line + 4;
            if (!fs::g_mounted) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No filesystem mounted.\n");
            } else {
                size_t size = 0;
                if (fs::read_file(filename, g_cat_buffer, sizeof(g_cat_buffer), &size)) {
                    term.set_color(vga::Color::LightGrey, vga::Color::Black);
                    term.write(g_cat_buffer);
                    if (size == 0 || g_cat_buffer[size - 1] != '\n') term.write("\n");
                } else {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("No such file: ");
                    term.write(filename);
                    term.write("\n");
                }
            }
        } else if (starts_with(line, "write ")) {
            const char* rest = line + 6;
            size_t name_len = 0;
            while (rest[name_len] != '\0' && rest[name_len] != ' ') name_len++;

            if (name_len == 0 || name_len >= fs::NAME_LEN) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("Usage: write <filename> <text>\n");
            } else if (!fs::g_mounted) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No filesystem mounted.\n");
            } else {
                char filename[fs::NAME_LEN];
                for (size_t i = 0; i < name_len; i++) filename[i] = rest[i];
                filename[name_len] = '\0';

                const char* content = rest + name_len;
                if (*content == ' ') content++; // skip the separating space

                if (fs::write_file(filename, content, strutil::length(content))) {
                    term.set_color(vga::Color::LightGreen, vga::Color::Black);
                    term.write("Wrote ");
                    term.write(filename);
                    term.write(" (");
                    print_uint(term, strutil::length(content));
                    term.write(" bytes)\n");
                } else {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Write failed (directory full or ATA error).\n");
                }
            }
        } else if (starts_with(line, "rm ")) {
            const char* filename = line + 3;
            if (!fs::g_mounted) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No filesystem mounted.\n");
            } else if (fs::delete_file(filename)) {
                term.set_color(vga::Color::LightGreen, vga::Color::Black);
                term.write("Deleted ");
                term.write(filename);
                term.write("\n");
            } else {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No such file: ");
                term.write(filename);
                term.write("\n");
            }
        } else if (line_is(line, "fsstat")) {
            term.set_color(vga::Color::LightCyan, vga::Color::Black);
            if (!fs::g_mounted) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No filesystem mounted.\n");
            } else {
                term.write("Free extents: ");
                print_uint(term, fs::free_extent_count());
                term.write("   Free sectors: ");
                print_uint(term, fs::free_sector_count());
                term.write("\n");
            }
        } else if (line_is(line, "lsdisk")) {
            static const char* drive_names[ata::MAX_DRIVES] = {
                "0 (primary master)", "1 (primary slave)",
                "2 (secondary master)", "3 (secondary slave)"
            };
            term.set_color(vga::Color::LightCyan, vga::Color::Black);
            term.write("Drives:\n");
            for (uint8_t d = 0; d < ata::MAX_DRIVES; d++) {
                term.write("  ");
                term.write(drive_names[d]);
                ata::DriveKind kind = ata::identify(d, nullptr);
                if (kind == ata::DriveKind::ABSENT) {
                    term.write(" - not present\n");
                } else if (kind == ata::DriveKind::ATAPI_OR_OTHER) {
                    term.write(" - present, not a plain ATA disk (e.g. ATAPI/CD-ROM)\n");
                } else if (fs::looks_like_liquidfs(d)) {
                    term.write(" - present, LiquidFS\n");
                } else {
                    term.write(" - present, blank/foreign ATA disk\n");
                }
            }
            term.write("Mounted: ");
            if (!fs::g_mounted) {
                term.write("(none)\n");
            } else if (fs::mounted_backend() == fs::Backend::USB_MSC) {
                term.write("USB mass-storage device\n");
            } else {
                term.write("drive ");
                print_uint(term, fs::mounted_drive());
                term.write("\n");
            }
        } else if (starts_with(line, "mount ")) {
            const char* arg = line + 6;
            if (line_is(arg, "usb")) {
                if (fs::mount_usb()) {
                    term.set_color(vga::Color::LightGreen, vga::Color::Black);
                    term.write("Mounted USB mass-storage device: ");
                    print_uint(term, fs::file_count());
                    term.write(" file(s).\n");
                } else {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Mount failed - no USB mass-storage device with a LiquidFS disk found. Previous mount (if any) is untouched.\n");
                }
            } else if (arg[0] < '0' || arg[0] > '3' || arg[1] != '\0') {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("Usage: mount <0-3>  (0=primary master, 1=primary slave, 2=secondary master, 3=secondary slave)  or  mount usb\n");
            } else {
                uint8_t drive = (uint8_t)(arg[0] - '0');
                if (fs::mount(drive)) {
                    term.set_color(vga::Color::LightGreen, vga::Color::Black);
                    term.write("Mounted drive ");
                    print_uint(term, drive);
                    term.write(": ");
                    print_uint(term, fs::file_count());
                    term.write(" file(s).\n");
                } else {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Mount failed - no disk there, or not a LiquidFS disk. Previous mount (if any) is untouched.\n");
                }
            }
        } else if (line_is(line, "unmount")) {
            if (!fs::g_mounted) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("Nothing mounted.\n");
            } else {
                fs::unmount();
                term.set_color(vga::Color::LightGreen, vga::Color::Black);
                term.write("Unmounted.\n");
            }
        } else if (line_is(line, "lspci") || starts_with(line, "lspci ")) {
            const char* arg = line_is(line, "lspci") ? "" : line + 6;
            bool filter_net = line_is(arg, "net");
            bool filter_usb = line_is(arg, "usb");
            if (arg[0] != '\0' && !filter_net && !filter_usb) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("Usage: lspci  or  lspci net  or  lspci usb\n");
            } else {
                pci::enumerate();
                term.set_color(vga::Color::LightCyan, vga::Color::Black);
                if (filter_net || filter_usb) {
                    uint32_t matched = 0;
                    for (uint32_t i = 0; i < pci::g_device_count; i++) {
                        const pci::DeviceInfo& d = pci::g_devices[i];
                        bool is_net = (d.class_code == pci::CLASS_NETWORK && d.subclass == pci::SUBCLASS_ETHERNET);
                        bool is_usb = (d.class_code == pci::CLASS_SERIAL_BUS && d.subclass == pci::SUBCLASS_USB);
                        if ((filter_net && is_net) || (filter_usb && is_usb)) {
                            print_pci_device(term, d);
                            matched++;
                        }
                    }
                    if (matched == 0) {
                        term.set_color(vga::Color::LightRed, vga::Color::Black);
                        term.write(filter_net ? "No Ethernet controller found.\n" : "No USB host controller found.\n");
                    }
                } else {
                    term.write("PCI devices (");
                    print_uint(term, pci::g_device_count);
                    term.write("):\n");
                    for (uint32_t i = 0; i < pci::g_device_count; i++) {
                        print_pci_device(term, pci::g_devices[i]);
                    }
                }
            }
        } else if (line_is(line, "nicprobe")) {
            nic::ProbeResult r = nic::probe();
            term.set_color(vga::Color::LightCyan, vga::Color::Black);

            if (!r.found) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No Ethernet controller found.\n");
            } else {
                term.write("Found Ethernet controller at ");
                print_uint(term, r.dev.bus);
                term.write(":");
                print_uint(term, r.dev.device);
                term.write(".");
                print_uint(term, r.dev.function);
                term.write(" (vendor=");
                print_hex(term, r.dev.vendor_id, 4);
                term.write(" device=");
                print_hex(term, r.dev.device_id, 4);
                term.write(")\n");

                if (!r.have_bar) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("  No valid BAR found on this controller.\n");
                } else {
                    term.write("  BAR");
                    print_uint(term, (uint32_t)r.bar_index);
                    term.write(": ");
                    term.write(r.bar.is_io ? "I/O " : "MEM ");
                    if (!r.bar.is_io && r.bar.is_64bit) term.write("64-bit ");
                    if (!r.bar.is_io && r.bar.prefetchable) term.write("prefetchable ");
                    term.write(" base=0x");
                    print_hex(term, (uint32_t)(r.bar.base >> 32), 8);
                    print_hex(term, (uint32_t)(r.bar.base & 0xFFFFFFFFu), 8);
                    term.write("  size=");
                    print_uint(term, r.bar.size);
                    term.write(" bytes\n");

                    if (!r.bar_is_mmio) {
                        term.set_color(vga::Color::LightRed, vga::Color::Black);
                        term.write("  BAR is I/O-mapped - register read not implemented for that yet.\n");
                    } else {
                        term.write("  CTRL=0x");
                        print_hex(term, r.ctrl, 8);
                        term.write("  STATUS=0x");
                        print_hex(term, r.status, 8);
                        term.write("\n  Link ");
                        term.write((r.status & nic::STATUS_LINK_UP) ? "UP" : "DOWN");
                        if (r.status & nic::STATUS_LINK_UP) {
                            term.write(", ");
                            term.write((r.status & nic::STATUS_FULL_DUPLEX) ? "full" : "half");
                            term.write(" duplex, ");
                            uint32_t speed = r.status & nic::STATUS_SPEED_MASK;
                            if (speed == nic::STATUS_SPEED_10) term.write("10Mb/s");
                            else if (speed == nic::STATUS_SPEED_100) term.write("100Mb/s");
                            else term.write("1000Mb/s");
                        } else if (r.status & nic::STATUS_PHYRA) {
                            term.write(" (PHY Reset Asserted - expected, this milestone never brings the PHY up)");
                        }
                        term.write("\n  MAC address: ");
                        if (!r.mac_valid) {
                            term.write("(RAL0/RAH0 slot not marked valid)");
                        } else {
                            for (int i = 0; i < 6; i++) {
                                print_hex(term, r.mac[i], 2);
                                if (i != 5) term.write(":");
                            }
                        }
                        term.write("\n");
                    }
                }
            }
        } else if (line_is(line, "usbprobe")) {
            pci::enumerate();
            term.set_color(vga::Color::LightCyan, vga::Color::Black);

            bool found_controller = false;
            for (uint32_t i = 0; i < pci::g_device_count; i++) {
                const pci::DeviceInfo& d = pci::g_devices[i];
                if (d.class_code != pci::CLASS_SERIAL_BUS || d.subclass != pci::SUBCLASS_USB) continue;
                found_controller = true;

                term.write("Found USB controller at ");
                print_uint(term, d.bus);
                term.write(":");
                print_uint(term, d.device);
                term.write(".");
                print_uint(term, d.function);
                term.write(" (");
                if (d.prog_if == pci::PROGIF_UHCI) term.write("UHCI");
                else if (d.prog_if == pci::PROGIF_OHCI) term.write("OHCI");
                else if (d.prog_if == pci::PROGIF_EHCI) term.write("EHCI");
                else if (d.prog_if == pci::PROGIF_XHCI) term.write("xHCI");
                else term.write("unknown type");
                term.write(")\n");

                // The kind of BAR this controller type actually uses:
                // UHCI/OHCI are I/O-mapped, EHCI/xHCI are memory-mapped.
                bool wants_io = (d.prog_if == pci::PROGIF_UHCI || d.prog_if == pci::PROGIF_OHCI);
                bool have_primary = false;
                pci::BarInfo primary{};
                int primary_index = -1;

                for (int b = 0; b < 6; b++) {
                    pci::BarInfo bar = pci::parse_bar(d.bus, d.device, d.function, b);
                    if (!bar.valid) continue;

                    term.write("  BAR");
                    print_uint(term, (uint32_t)b);
                    term.write(": ");
                    term.write(bar.is_io ? "I/O " : "MEM ");
                    if (!bar.is_io && bar.is_64bit) term.write("64-bit ");
                    if (!bar.is_io && bar.prefetchable) term.write("prefetchable ");
                    term.write(" base=0x");
                    print_hex(term, (uint32_t)(bar.base >> 32), 8);
                    print_hex(term, (uint32_t)(bar.base & 0xFFFFFFFFu), 8);
                    term.write("  size=");
                    print_uint(term, bar.size);
                    term.write(" bytes\n");

                    if (!have_primary && bar.is_io == wants_io) {
                        have_primary = true;
                        primary = bar;
                        primary_index = b;
                    }
                }

                if (!have_primary) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("  No usable BAR found for this controller type.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                term.write("  Using BAR");
                print_uint(term, (uint32_t)primary_index);
                term.write(" as the register base.\n  ");

                if (d.prog_if == pci::PROGIF_UHCI) {
                    uint16_t io_base = (uint16_t)primary.base;
                    uint16_t usbcmd = io_inw((uint16_t)(io_base + 0x00));
                    uint16_t usbsts = io_inw((uint16_t)(io_base + 0x02));
                    term.write("USBCMD=0x");
                    print_hex(term, usbcmd, 4);
                    term.write("  USBSTS=0x");
                    print_hex(term, usbsts, 4);
                    term.write("\n");
                } else if (d.prog_if == pci::PROGIF_EHCI) {
                    uint32_t cap_reg = mmio::read32(primary.base);
                    uint8_t cap_length = (uint8_t)(cap_reg & 0xFF);
                    uint16_t hci_version = (uint16_t)((cap_reg >> 16) & 0xFFFF);
                    term.write("CAPLENGTH=0x");
                    print_hex(term, cap_length, 2);
                    term.write("  HCIVERSION=0x");
                    print_hex(term, hci_version, 4);
                    term.write("\n");
                } else {
                    term.write("(register read not implemented yet for this controller type)\n");
                }
            }

            if (!found_controller) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No USB controller found on the PCI bus.\n");
            }
        } else if (line_is(line, "usbup")) {
            pci::enumerate();
            term.set_color(vga::Color::LightCyan, vga::Color::Black);

            bool found_ehci = false;
            for (uint32_t i = 0; i < pci::g_device_count; i++) {
                const pci::DeviceInfo& d = pci::g_devices[i];
                if (d.class_code != pci::CLASS_SERIAL_BUS || d.subclass != pci::SUBCLASS_USB) continue;
                if (d.prog_if != pci::PROGIF_EHCI) continue; // this milestone only brings up EHCI - see ehci.h
                found_ehci = true;

                term.write("Bringing up EHCI controller at ");
                print_uint(term, d.bus);
                term.write(":");
                print_uint(term, d.device);
                term.write(".");
                print_uint(term, d.function);
                term.write("\n");

                ehci::Controller controller;
                if (!ehci::bring_up(d, &controller)) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("  Bring-up failed (BAR invalid, or reset/start timed out).\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                term.set_color(vga::Color::LightGreen, vga::Color::Black);
                term.write("  Controller reset and running. Ports: ");
                print_uint(term, controller.num_ports);
                term.write("\n");
                term.set_color(vga::Color::LightCyan, vga::Color::Black);

                for (uint32_t p = 0; p < controller.num_ports; p++) {
                    uint32_t portsc = ehci::read_portsc(controller.op_base, p);
                    bool connected = (portsc & ehci::PORTSC_CURRENT_CONNECT_STATUS) != 0;

                    term.write("  Port ");
                    print_uint(term, p);
                    term.write(": ");
                    if (!connected) {
                        term.write("no device\n");
                        continue;
                    }

                    term.write("device connected, resetting... ");
                    ehci::PortStatus status = ehci::reset_port(controller.op_base, p);
                    if (status.enabled) {
                        term.set_color(vga::Color::LightGreen, vga::Color::Black);
                        term.write("enabled (high-speed, owned by EHCI)\n");
                    } else if (status.owned_by_companion) {
                        term.set_color(vga::Color::Yellow, vga::Color::Black);
                        term.write("handed off to companion controller (full/low-speed device)\n");
                    } else {
                        term.set_color(vga::Color::LightRed, vga::Color::Black);
                        term.write("did not enable (reset failed or timed out)\n");
                    }
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                }
            }

            if (!found_ehci) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No EHCI controller found on the PCI bus.\n");
            }
        } else if (line_is(line, "usbenum")) {
            pci::enumerate();
            term.set_color(vga::Color::LightCyan, vga::Color::Black);

            bool found_ehci = false;
            for (uint32_t i = 0; i < pci::g_device_count; i++) {
                const pci::DeviceInfo& d = pci::g_devices[i];
                if (d.class_code != pci::CLASS_SERIAL_BUS || d.subclass != pci::SUBCLASS_USB) continue;
                if (d.prog_if != pci::PROGIF_EHCI) continue;
                found_ehci = true;

                ehci::Controller controller;
                if (!ehci::bring_up(d, &controller)) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Bring-up failed.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                bool device_ready = false;
                uint32_t ready_port = 0;
                for (uint32_t p = 0; p < controller.num_ports; p++) {
                    uint32_t portsc = ehci::read_portsc(controller.op_base, p);
                    if ((portsc & ehci::PORTSC_CURRENT_CONNECT_STATUS) == 0) continue;

                    ehci::PortStatus status = ehci::reset_port(controller.op_base, p);
                    if (status.enabled) {
                        device_ready = true;
                        ready_port = p;
                        break;
                    }
                }

                if (!device_ready) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("No enabled (high-speed) device found on any port.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                term.write("Device enabled on port ");
                print_uint(term, ready_port);
                term.write(" - enumerating...\n");

                if (!ehci::enable_async_schedule(controller)) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Failed to enable the asynchronous schedule.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }
                print_async_enable_status(term, controller, d);

                usb::DeviceInfo info;
                if (!usb::enumerate(controller, 1, &info)) {
                    print_enum_failure(term, controller, d);
                    continue;
                }

                term.set_color(vga::Color::LightGreen, vga::Color::Black);
                term.write("Enumerated successfully:\n");
                term.set_color(vga::Color::LightCyan, vga::Color::Black);
                term.write("  Address: ");
                print_uint(term, info.address);
                term.write("   Max packet size (EP0): ");
                print_uint(term, info.max_packet_size0);
                term.write("\n  Vendor=0x");
                print_hex(term, info.vendor_id, 4);
                term.write(" Product=0x");
                print_hex(term, info.product_id, 4);
                term.write("  Device class=0x");
                print_hex(term, info.device_class, 2);
                term.write("\n  Interface class=0x");
                print_hex(term, info.interface_class, 2);
                term.write(" subclass=0x");
                print_hex(term, info.interface_subclass, 2);
                term.write(" protocol=0x");
                print_hex(term, info.interface_protocol, 2);
                if (info.interface_class == usb::USB_CLASS_MASS_STORAGE) {
                    term.set_color(vga::Color::Yellow, vga::Color::Black);
                    term.write("  <- mass storage");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                }
                term.write("\n  Bulk IN endpoint: ");
                if (info.has_bulk_in) {
                    print_uint(term, info.bulk_in_endpoint);
                    term.write(" (max packet ");
                    print_uint(term, info.bulk_in_max_packet);
                    term.write(")");
                } else {
                    term.write("none");
                }
                term.write("\n  Bulk OUT endpoint: ");
                if (info.has_bulk_out) {
                    print_uint(term, info.bulk_out_endpoint);
                    term.write(" (max packet ");
                    print_uint(term, info.bulk_out_max_packet);
                    term.write(")");
                } else {
                    term.write("none");
                }
                term.write("\n");
            }

            if (!found_ehci) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No EHCI controller found on the PCI bus.\n");
            }
        } else if (line_is(line, "usbmsc")) {
            pci::enumerate();
            term.set_color(vga::Color::LightCyan, vga::Color::Black);

            bool found_ehci = false;
            for (uint32_t i = 0; i < pci::g_device_count; i++) {
                const pci::DeviceInfo& d = pci::g_devices[i];
                if (d.class_code != pci::CLASS_SERIAL_BUS || d.subclass != pci::SUBCLASS_USB) continue;
                if (d.prog_if != pci::PROGIF_EHCI) continue;
                found_ehci = true;

                ehci::Controller controller;
                if (!ehci::bring_up(d, &controller)) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Bring-up failed.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                bool device_ready = false;
                uint32_t ready_port = 0;
                for (uint32_t p = 0; p < controller.num_ports; p++) {
                    uint32_t portsc = ehci::read_portsc(controller.op_base, p);
                    if ((portsc & ehci::PORTSC_CURRENT_CONNECT_STATUS) == 0) continue;
                    ehci::PortStatus status = ehci::reset_port(controller.op_base, p);
                    if (status.enabled) {
                        device_ready = true;
                        ready_port = p;
                        break;
                    }
                }

                if (!device_ready) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("No enabled (high-speed) device found on any port.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }
                (void)ready_port;

                if (!ehci::enable_async_schedule(controller)) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Failed to enable the asynchronous schedule.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }
                print_async_enable_status(term, controller, d);

                usb::DeviceInfo info;
                if (!usb::enumerate(controller, 1, &info)) {
                    print_enum_failure(term, controller, d);
                    continue;
                }

                if (!info.has_bulk_in || !info.has_bulk_out) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Device has no bulk IN/OUT pair - not usable as mass storage.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                if (info.interface_class != usb::USB_CLASS_MASS_STORAGE) {
                    term.set_color(vga::Color::Yellow, vga::Color::Black);
                    term.write("Warning: interface class 0x");
                    print_hex(term, info.interface_class, 2);
                    term.write(" isn't 0x08 (mass storage) - trying SCSI commands anyway.\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                }

                ehci::reset_bulk_toggles(); // a freshly configured endpoint always starts at DATA0

                msc::Device dev{};
                dev.address = info.address;
                dev.bulk_in_endpoint = info.bulk_in_endpoint;
                dev.bulk_in_max_packet = info.bulk_in_max_packet;
                dev.bulk_out_endpoint = info.bulk_out_endpoint;
                dev.bulk_out_max_packet = info.bulk_out_max_packet;
                dev.next_tag = 1;

                term.write("TEST UNIT READY: ");
                if (msc::test_unit_ready(dev)) {
                    term.set_color(vga::Color::LightGreen, vga::Color::Black);
                    term.write("ready\n");
                } else {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("not ready / failed\n");
                }
                term.set_color(vga::Color::LightCyan, vga::Color::Black);

                msc::InquiryData inq;
                if (msc::inquiry(dev, &inq)) {
                    char vendor[9], product[17];
                    __builtin_memcpy(vendor, inq.vendor_id, 8); vendor[8] = '\0';
                    __builtin_memcpy(product, inq.product_id, 16); product[16] = '\0';
                    term.write("INQUIRY: vendor=\"");
                    term.write(vendor);
                    term.write("\" product=\"");
                    term.write(product);
                    term.write("\"\n");
                } else {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("INQUIRY failed\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                msc::Capacity cap;
                if (!msc::read_capacity(dev, &cap)) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("READ CAPACITY failed\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }
                term.write("READ CAPACITY: ");
                print_uint(term, cap.last_lba + 1);
                term.write(" blocks x ");
                print_uint(term, cap.block_size);
                term.write(" bytes\n");

                constexpr uint32_t MAX_TEST_BLOCK_SIZE = 4096; // matches ehci::bulk_transfer()'s bounce-buffer cap
                if (cap.block_size == 0 || cap.block_size > MAX_TEST_BLOCK_SIZE) {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("Block size unusable for this test (0 or too large).\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                static uint8_t sector_buf[MAX_TEST_BLOCK_SIZE];
                if (msc::read10(dev, 0, 1, cap.block_size, sector_buf)) {
                    term.set_color(vga::Color::LightGreen, vga::Color::Black);
                    term.write("READ(10) LBA 0: \"");
                    for (uint32_t c = 0; c < cap.block_size && c < 64; c++) {
                        char ch = (char)sector_buf[c];
                        term.write_char((ch >= 32 && ch < 127) ? ch : '.');
                    }
                    term.write("\"\n");
                } else {
                    term.set_color(vga::Color::LightRed, vga::Color::Black);
                    term.write("READ(10) failed\n");
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                    continue;
                }

                // Round-trip a WRITE(10) against a SCRATCH sector (LBA 1,
                // never LBA 0) to prove writes genuinely persist, not
                // just reads: write a known pattern, then read the same
                // sector back and compare byte-for-byte.
                if (cap.last_lba >= 1) {
                    static uint8_t write_buf[MAX_TEST_BLOCK_SIZE];
                    for (uint32_t c = 0; c < cap.block_size; c++) {
                        write_buf[c] = (uint8_t)('A' + (c % 26));
                    }
                    bool roundtrip_ok = false;
                    if (msc::write10(dev, 1, 1, cap.block_size, write_buf)) {
                        static uint8_t readback_buf[MAX_TEST_BLOCK_SIZE];
                        if (msc::read10(dev, 1, 1, cap.block_size, readback_buf)) {
                            // A hand-rolled loop, not __builtin_memcmp() with
                            // a runtime size - the compiler lowers that to a
                            // real memcmp() call, which this freestanding
                            // kernel has no libc to provide (same reasoning
                            // as fs.h's constant-size-memcpy comments).
                            roundtrip_ok = true;
                            for (uint32_t c = 0; c < cap.block_size; c++) {
                                if (write_buf[c] != readback_buf[c]) { roundtrip_ok = false; break; }
                            }
                        }
                    }
                    term.write("WRITE(10)+READ(10) round-trip on LBA 1: ");
                    if (roundtrip_ok) {
                        term.set_color(vga::Color::LightGreen, vga::Color::Black);
                        term.write("match\n");
                    } else {
                        term.set_color(vga::Color::LightRed, vga::Color::Black);
                        term.write("MISMATCH or failed\n");
                    }
                    term.set_color(vga::Color::LightCyan, vga::Color::Black);
                }
            }

            if (!found_ehci) {
                term.set_color(vga::Color::LightRed, vga::Color::Black);
                term.write("No EHCI controller found on the PCI bus.\n");
            }
        } else if (line_is(line, "de")) {
            trigger_divide_by_zero();
        } else if (line_is(line, "pf")) {
            trigger_page_fault();
        } else if (line_is(line, "help")) {
            term.write("Commands: ticks, tasktest, preempttest, heaptest, heapstats, ls, cat <file>, write <file> <text>, rm <file>, fsstat, lsdisk, mount <0-3>, unmount, lspci, lspci net, lspci usb, usbprobe, usbup, usbenum, usbmsc, nicprobe, de, pf, help\n");
        } else if (line_is(line, "")) {
            // ignore
        } else {
            term.set_color(vga::Color::LightRed, vga::Color::Black);
            term.write("Unknown command. Type 'help' for the list.\n");
        }
    }
}
