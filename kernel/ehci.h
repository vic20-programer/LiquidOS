// ehci.h — brings up an EHCI (USB 2.0) host controller (BIOS-to-OS
// handoff, reset, start, port reset — see each function's comment) and,
// as of the USB-device-enumeration milestone, submits real control
// transfers over its asynchronous schedule: enable_async_schedule(),
// QueueHead/QueueTD (the DMA-visible structures EHCI itself reads to
// execute a transfer), and control_transfer(), which builds the
// SETUP/DATA/STATUS qTD chain for one control transfer and busy-waits
// for the hardware to finish it.
//
// EHCI only, deliberately: the ProBook 6450b this project keeps
// testing against is old enough that EHCI (possibly with UHCI
// companion controllers for full/low-speed passthrough) is by far the
// most likely real target, and building matching logic for UHCI/OHCI/
// xHCI as well would roughly double this file's size for controller
// types that may never actually matter here. If it turns out this
// hardware needs one of those instead, that's a concrete, specific
// problem to solve then — not a hypothetical to build against now.
//
// What's still NOT here: USB protocol semantics (descriptor layouts,
// what a GET_DESCRIPTOR request actually looks like, the enumeration
// sequence) — see usb.h, which is built entirely on top of
// control_transfer() and never touches EHCI registers directly itself.

#pragma once
#include <stdint.h>
#include "mmio.h"
#include "pci.h"
#include "irq.h"

namespace ehci {

// Every busy-wait loop in this file spins on this between checks. This
// found to matter more than it looks: under QEMU/TCG specifically, a
// tight guest spin loop doing rapid MMIO reads with no `pause` can
// starve QEMU's own device-emulation processing of the chance to
// actually advance the virtual controller's async schedule state
// machine, turning a poll that should resolve in microseconds into one
// that never resolves within this file's timeouts at all - and, worse,
// non-deterministically so (some runs got lucky with vCPU/IO-thread
// scheduling, most didn't). PAUSE is the standard x86 spin-loop
// instruction for exactly this reason: it's a hint to the CPU (and, in
// a virtualized guest, to the hypervisor/emulator) that this is a
// wait-for-memory-to-change loop, not real work — real hardware
// benefits from the power/contention improvement PAUSE was designed
// for; under emulation, the induced pause between iterations was what
// actually gave QEMU's device-emulation code room to run at all.
inline void cpu_pause() {
    asm volatile("pause");
}

// Capability registers - read-only, start at the BAR's base address.
constexpr uint32_t CAPLENGTH_OFFSET  = 0x00; // u8: operational registers start at base + this
constexpr uint32_t HCIVERSION_OFFSET = 0x02; // u16
constexpr uint32_t HCSPARAMS_OFFSET  = 0x04; // u32: bits 3:0 = number of root hub ports
constexpr uint32_t HCCPARAMS_OFFSET  = 0x08; // u32: bits 15:8 = EECP (0 = no extended capabilities)

// Operational registers - start at BAR base + CAPLENGTH.
constexpr uint32_t USBCMD_OFFSET           = 0x00;
constexpr uint32_t USBSTS_OFFSET           = 0x04;
constexpr uint32_t USBINTR_OFFSET          = 0x08;
constexpr uint32_t FRINDEX_OFFSET          = 0x0C; // u32: frame index counter - advances continuously once the controller's schedule engine is actually running, regardless of DMA correctness
constexpr uint32_t CTRLDSSEGMENT_OFFSET    = 0x10; // high 32 bits for ALL of this controller's DMA addresses, if it supports 64-bit addressing (HCCPARAMS bit 0) - see reset_controller()'s comment
constexpr uint32_t PERIODICLISTBASE_OFFSET = 0x14; // only supposed to matter when the periodic schedule is enabled (it never is here) - see reset_controller()'s comment on why it's initialized anyway
constexpr uint32_t ASYNCLISTADDR_OFFSET    = 0x18;
constexpr uint32_t CONFIGFLAG_OFFSET       = 0x40;
constexpr uint32_t PORTSC_BASE_OFFSET      = 0x44;

constexpr uint32_t USBCMD_RUN_STOP                 = 1u << 0;
constexpr uint32_t USBCMD_HCRESET                  = 1u << 1;
constexpr uint32_t USBCMD_PERIODIC_SCHEDULE_ENABLE = 1u << 4;
constexpr uint32_t USBCMD_ASYNC_SCHEDULE_ENABLE    = 1u << 5;
constexpr uint32_t USBSTS_USB_ERROR_INT         = 1u << 1;  // latches on any transaction completing with an error, even though we never unmask interrupts
constexpr uint32_t USBSTS_FRAME_LIST_ROLLOVER   = 1u << 3;
constexpr uint32_t USBSTS_HOST_SYSTEM_ERROR     = 1u << 4;  // hardware hit a serious error (e.g. a PCI-level fault) accessing a data structure - THE bit that would confirm a DMA/bus problem directly, never checked before
constexpr uint32_t USBSTS_HCHALTED              = 1u << 12;
constexpr uint32_t USBSTS_ASYNC_SCHEDULE_STATUS = 1u << 15;
constexpr uint32_t USBSTS_STATUS_CLEAR_MASK     = 0x3Fu; // every W1C event bit (0-5) - a real, working reference driver clears these explicitly before starting rather than trusting reset to have done it
constexpr uint32_t HCCPARAMS_64BIT_ADDRESSING   = 1u << 0;

constexpr uint32_t PORTSC_CURRENT_CONNECT_STATUS = 1u << 0;
constexpr uint32_t PORTSC_CONNECT_STATUS_CHANGE  = 1u << 1;
constexpr uint32_t PORTSC_PORT_ENABLED           = 1u << 2;
constexpr uint32_t PORTSC_PORT_ENABLE_CHANGE     = 1u << 3;
constexpr uint32_t PORTSC_PORT_RESET             = 1u << 8;
constexpr uint32_t PORTSC_PORT_OWNER             = 1u << 13;
// The only two write-1-to-clear status bits in PORTSC - a naive
// read-modify-write risks silently clearing whichever of these happens
// to be set at the time, since writing back a 1 that was only ever
// READ (not intentionally set) clears it. Every PORTSC write in this
// file masks these out first unless a change is specifically being
// acknowledged.
constexpr uint32_t PORTSC_RWC_MASK = PORTSC_CONNECT_STATUS_CHANGE | PORTSC_PORT_ENABLE_CHANGE;

// USB Legacy Support Extended Capability, in PCI CONFIGURATION space
// (not MMIO) at the offset HCCPARAMS' EECP field points to - this is a
// different addressing space from everything else in this file.
constexpr uint8_t USB_LEGACY_SUPPORT_CAP_ID = 0x01;
constexpr uint32_t USBLEGSUP_OS_OWNED   = 1u << 16;
constexpr uint32_t USBLEGSUP_BIOS_OWNED = 1u << 24;

// Bounds, all in PIT ticks (100Hz as of this milestone - see
// irq::g_tick_count). Matches ata.h's convention of never spinning
// forever on hardware that might not respond the way it's supposed to.
constexpr uint64_t HANDOFF_TIMEOUT_TICKS = 500; // ~5s - generous margin for a BIOS to release ownership
constexpr uint64_t RESET_TIMEOUT_TICKS   = 200; // ~2s - reset/start should complete in milliseconds on real hardware
constexpr uint64_t PORT_RESET_HOLD_TICKS = 6;   // >=50ms, the USB 2.0-mandated minimum reset pulse width (60ms here, a safe margin)
constexpr uint64_t RESET_RECOVERY_TICKS  = 3;   // >=10ms, the USB 2.0-mandated "reset recovery time" before the first request (30ms here, a safe margin)

inline uint32_t phys32(const volatile void* p) {
    return (uint32_t)(uintptr_t)p; // safe: this kernel loads and identity-maps well within 32-bit physical address space
}

constexpr uint32_t FRAME_LIST_TERMINATE = 1u << 0; // "T" bit - same encoding as QTD_TERMINATE, just named for this context

// A valid (every entry Terminate-flagged, "nothing scheduled") periodic
// frame list. PERIODICLISTBASE is only supposed to matter when the
// periodic schedule is enabled (USBCMD's PSE bit - never set anywhere in
// this file, see this file's own top comment), so leaving this register
// uninitialized should be harmless per spec. After several straight
// rounds of real-hardware-only EHCI failures that all turned out to
// matter despite the spec suggesting otherwise (see kernel.cpp's usbmsc
// diagnostic history), this is the one remaining operational register
// this driver never writes at all - cheap and safe to initialize anyway.
// 1024 entries * 4 bytes = 4096 bytes, matching the default Frame List
// Size (USBCMD bits 3:2 = 00), which this file also never changes.
alignas(4096) inline uint32_t g_periodic_frame_list[1024];

inline uint32_t read_portsc(uint64_t op_base, uint32_t port_index) {
    return mmio::read32(op_base + PORTSC_BASE_OFFSET + port_index * 4);
}

// Sets exactly `bits_to_set` in PORTSC without disturbing the RWC
// status bits (see PORTSC_RWC_MASK) unless they're explicitly part of
// bits_to_set - i.e. acknowledging a change means passing it in
// bits_to_set on purpose, not as a side effect of some other bit
// happening to already be there when read.
inline void set_portsc_bits(uint64_t op_base, uint32_t port_index, uint32_t bits_to_set) {
    uint32_t current = read_portsc(op_base, port_index);
    uint32_t preserved = current & ~PORTSC_RWC_MASK;
    mmio::write32(op_base + PORTSC_BASE_OFFSET + port_index * 4, preserved | bits_to_set);
}

inline void clear_portsc_bits(uint64_t op_base, uint32_t port_index, uint32_t bits_to_clear) {
    uint32_t current = read_portsc(op_base, port_index);
    uint32_t preserved = current & ~PORTSC_RWC_MASK;
    mmio::write32(op_base + PORTSC_BASE_OFFSET + port_index * 4, preserved & ~bits_to_clear);
}

// Walks the EHCI Extended Capabilities linked list (in PCI
// configuration space, starting at HCCPARAMS' EECP field) looking for
// the USB Legacy Support capability, and — if the BIOS currently claims
// ownership — requests it for the OS and waits for the BIOS to release
// it. Best-effort: if the BIOS never responds within
// HANDOFF_TIMEOUT_TICKS, proceeds anyway rather than refusing to bring
// the controller up over an unresponsive/buggy BIOS, matching common
// real-world driver practice. A no-op if this controller has no such
// capability at all (EECP == 0, e.g. QEMU's emulated EHCI) - there's
// nothing to hand off in that case.
enum class HandoffResult : uint8_t { NO_CAPABILITY, ALREADY_OS_OWNED, BIOS_RELEASED, BIOS_TIMED_OUT };

// Diagnostic-only, paired with g_last_legsup_cap_offset (0 if no capability
// was found at all) - lets a caller re-read the USB Legacy Support
// capability dword LATER (e.g. right when a transfer fails) to check
// whether BIOS_OWNED has come back since handoff, which would mean the
// BIOS's own SMM code is still actively touching this controller after
// we thought we'd taken it over - not something bios_handoff() itself can
// detect, since it only watches during its own timeout window.
inline HandoffResult g_last_handoff_result = HandoffResult::NO_CAPABILITY;
inline uint8_t g_last_legsup_cap_offset = 0;

inline void bios_handoff(const pci::DeviceInfo& dev, uint64_t bar_base) {
    uint32_t hccparams = mmio::read32(bar_base + HCCPARAMS_OFFSET);
    uint8_t next = (uint8_t)((hccparams >> 8) & 0xFF);

    g_last_handoff_result = HandoffResult::NO_CAPABILITY;
    g_last_legsup_cap_offset = 0;

    while (next >= 0x40) { // < 0x40 isn't a valid extended-capability offset - treat as "no more"
        uint32_t cap = pci::read_config_dword(dev.bus, dev.device, dev.function, next);
        uint8_t cap_id = (uint8_t)(cap & 0xFF);
        uint8_t next_ptr = (uint8_t)((cap >> 8) & 0xFF);

        if (cap_id == USB_LEGACY_SUPPORT_CAP_ID) {
            g_last_legsup_cap_offset = next;
            if (cap & USBLEGSUP_BIOS_OWNED) {
                pci::write_config_dword(dev.bus, dev.device, dev.function, next, cap | USBLEGSUP_OS_OWNED);

                uint64_t deadline = irq::g_tick_count + HANDOFF_TIMEOUT_TICKS;
                g_last_handoff_result = HandoffResult::BIOS_TIMED_OUT;
                while (irq::g_tick_count < deadline) {
                    uint32_t now = pci::read_config_dword(dev.bus, dev.device, dev.function, next);
                    if ((now & USBLEGSUP_BIOS_OWNED) == 0) { g_last_handoff_result = HandoffResult::BIOS_RELEASED; break; }
                    cpu_pause();
                }
                // Timed out or not - either way, move on. A BIOS that
                // never lets go is a BIOS bug, not something worth
                // refusing to boot over.
            } else {
                g_last_handoff_result = HandoffResult::ALREADY_OS_OWNED;
            }
            return;
        }

        next = next_ptr;
    }
}

// Stops the controller if running, asserts HCRESET, and waits for the
// controller to clear it - the spec requires this before touching any
// other operational register. Bounded: a controller that never clears
// either bit is treated as failed, not waited on forever.
inline bool reset_controller(uint64_t op_base) {
    uint32_t cmd = mmio::read32(op_base + USBCMD_OFFSET);
    mmio::write32(op_base + USBCMD_OFFSET, cmd & ~USBCMD_RUN_STOP);

    uint64_t deadline = irq::g_tick_count + RESET_TIMEOUT_TICKS;
    while ((mmio::read32(op_base + USBSTS_OFFSET) & USBSTS_HCHALTED) == 0) {
        if (irq::g_tick_count >= deadline) return false;
        cpu_pause();
    }

    mmio::write32(op_base + USBCMD_OFFSET, USBCMD_HCRESET);

    deadline = irq::g_tick_count + RESET_TIMEOUT_TICKS;
    while ((mmio::read32(op_base + USBCMD_OFFSET) & USBCMD_HCRESET) != 0) {
        if (irq::g_tick_count >= deadline) return false;
        cpu_pause();
    }

    // If this controller supports 64-bit addressing (HCCPARAMS bit 0), it
    // pairs EVERY DMA address it's given - ASYNCLISTADDR, every qTD's
    // buffer/link pointers, all of it, not just addresses that actually
    // need bits above 32 - with whatever's in this register as the high
    // 32 bits. Every physical address this driver ever hands the
    // controller is 32-bit (phys32()), so this must read as zero or the
    // controller will go looking for our queue heads at a completely
    // different, likely nonexistent physical address and never touch
    // them - indistinguishable from the controller simply never running
    // the schedule at all. Not guaranteed zero after HCRESET on real
    // silicon (unlike QEMU's emulated controller, which doesn't implement
    // 64-bit addressing and so never consults this register regardless of
    // its value) - explicit, not relied on.
    mmio::write32(op_base + CTRLDSSEGMENT_OFFSET, 0);

    // A real, working from-scratch EHCI driver explicitly clears every
    // USBSTS event bit here rather than trusting HCRESET to have done
    // it - found by diffing against reference driver source after 10+
    // real-hardware-only rounds on this project's own Master Abort bug
    // (see kernel.cpp's usbmsc diagnostic history). Belt-and-suspenders:
    // makes sure nothing stale is left over before this controller does
    // anything else.
    mmio::write32(op_base + USBSTS_OFFSET, USBSTS_STATUS_CLEAR_MASK);

    // See g_periodic_frame_list's comment above.
    for (int i = 0; i < 1024; i++) g_periodic_frame_list[i] = FRAME_LIST_TERMINATE;
    mmio::write32(op_base + PERIODICLISTBASE_OFFSET, phys32(g_periodic_frame_list));

    return true;
}

// Disables interrupts (this project is still PIO-polling-only - see
// ata.h's original design note), routes every port to this EHCI
// controller instead of a companion controller (CONFIGFLAG), and sets
// Run/Stop. Waits for USBSTS.HCHalted to clear, confirming the
// controller is genuinely running rather than just having accepted the
// register write.
inline bool start_controller(uint64_t op_base) {
    mmio::write32(op_base + USBINTR_OFFSET, 0);
    mmio::write32(op_base + CONFIGFLAG_OFFSET, 1);

    uint32_t cmd = mmio::read32(op_base + USBCMD_OFFSET);
    mmio::write32(op_base + USBCMD_OFFSET, cmd | USBCMD_RUN_STOP);

    uint64_t deadline = irq::g_tick_count + RESET_TIMEOUT_TICKS;
    while ((mmio::read32(op_base + USBSTS_OFFSET) & USBSTS_HCHALTED) != 0) {
        if (irq::g_tick_count >= deadline) return false;
        cpu_pause();
    }
    return true;
}

struct Controller {
    uint64_t bar_base;
    uint32_t bar_size;
    uint64_t op_base;   // bar_base + CAPLENGTH - where operational registers begin
    uint32_t num_ports; // from HCSPARAMS bits 3:0
};

// Defined below (needs the qTD/QH static structures declared later in
// this file) - forward-declared here since bring_up() calls it.
inline void mark_control_structures_uncacheable();

// Runs the full bring-up sequence: BIOS handoff, reset, start. Returns
// false (controller left in whatever partial state the failing step
// left it in) if reset or start times out; true, with `out` filled in,
// once the controller is confirmed running.
inline bool bring_up(const pci::DeviceInfo& dev, Controller* out) {
    pci::BarInfo bar0 = pci::parse_bar(dev.bus, dev.device, dev.function, 0);
    if (!bar0.valid || bar0.is_io) return false; // not the MMIO BAR this controller type should have

    // Must happen before the FIRST MMIO access to this BAR, below - see
    // mmio.h's mark_uncacheable()/top-of-file comment. Without this,
    // boot.asm's identity map leaves this region ordinary write-back
    // cacheable, and a real controller's registers (which change on
    // their own, asynchronously) can appear to freeze at a stale cached
    // value indefinitely - invisible under QEMU/TCG, which doesn't
    // meaningfully emulate CPU caching.
    mmio::mark_uncacheable(bar0.base, bar0.size);

    // See mark_control_structures_uncacheable()'s comment - the other
    // half of the cache-coherency question, for the structures the
    // controller reads/writes via DMA rather than the ones it exposes
    // as MMIO registers.
    mark_control_structures_uncacheable();

    // Must happen before anything that depends on the controller's own DMA
    // (i.e. everything past reset - see pci::enable_bus_mastering()'s
    // comment). Register-level reset/start/status polling below would all
    // still appear to work without this; only the async schedule actually
    // fetching a queue head would silently never happen.
    pci::enable_bus_mastering(dev.bus, dev.device, dev.function);

    uint8_t cap_length = mmio::read8(bar0.base + CAPLENGTH_OFFSET);
    uint64_t op_base = bar0.base + cap_length;

    bios_handoff(dev, bar0.base);

    if (!reset_controller(op_base)) return false;

    uint32_t hcsparams = mmio::read32(bar0.base + HCSPARAMS_OFFSET);
    uint32_t num_ports = hcsparams & 0xF;

    if (!start_controller(op_base)) return false;

    out->bar_base = bar0.base;
    out->bar_size = bar0.size;
    out->op_base = op_base;
    out->num_ports = num_ports;
    return true;
}

struct PortStatus {
    bool connected;
    bool enabled;            // true: a high-speed device, owned by this EHCI controller
    bool owned_by_companion; // true: EHCI released this port to a companion UHCI/OHCI controller (normal for full/low-speed devices)
};

// Performs the standard USB port reset handshake on `port_index`: if a
// device is connected, acknowledges the connect event, asserts Port
// Reset for at least the USB 2.0-mandated 50ms, then deasserts it and
// waits for the controller to confirm. A device that comes back
// Enabled is high-speed and ready for a later milestone's enumeration;
// one that doesn't has been silently handed to a companion controller
// instead (PORTSC_PORT_OWNER) - both are normal, expected outcomes, not
// errors, since EHCI only ever handles high-speed devices itself.
inline PortStatus reset_port(uint64_t op_base, uint32_t port_index) {
    PortStatus status{};

    uint32_t before = read_portsc(op_base, port_index);
    status.connected = (before & PORTSC_CURRENT_CONNECT_STATUS) != 0;
    if (!status.connected) return status;

    set_portsc_bits(op_base, port_index, PORTSC_CONNECT_STATUS_CHANGE); // acknowledge the connect event

    set_portsc_bits(op_base, port_index, PORTSC_PORT_RESET);

    uint64_t hold_until = irq::g_tick_count + PORT_RESET_HOLD_TICKS;
    while (irq::g_tick_count < hold_until) {
        // Busy-wait for the mandated minimum reset pulse width.
        cpu_pause();
    }

    clear_portsc_bits(op_base, port_index, PORTSC_PORT_RESET);

    uint64_t deadline = irq::g_tick_count + RESET_TIMEOUT_TICKS;
    while ((read_portsc(op_base, port_index) & PORTSC_PORT_RESET) != 0) {
        if (irq::g_tick_count >= deadline) break; // give up waiting, but still report whatever state exists below
        cpu_pause();
    }

    uint32_t after = read_portsc(op_base, port_index);
    status.enabled = (after & PORTSC_PORT_ENABLED) != 0;
    status.owned_by_companion = (after & PORTSC_PORT_OWNER) != 0;

    // USB 2.0 spec section 9.2.6.2's "reset recovery time": software
    // must wait at least 10ms after reset signaling ends before the
    // first request to the device, to give it time to reach the
    // Default state and be ready to respond.
    uint64_t recovery_until = irq::g_tick_count + RESET_RECOVERY_TICKS;
    while (irq::g_tick_count < recovery_until) {
        cpu_pause();
    }

    return status;
}

// --- Asynchronous schedule: submitting real control transfers -------------

constexpr uint32_t QTD_STATUS_ACTIVE            = 1u << 7;
constexpr uint32_t QTD_STATUS_HALTED            = 1u << 6;
constexpr uint32_t QTD_STATUS_BUFFER_ERROR      = 1u << 5;
constexpr uint32_t QTD_STATUS_BABBLE            = 1u << 4;
constexpr uint32_t QTD_STATUS_TRANSACTION_ERROR = 1u << 3;
constexpr uint32_t QTD_ERROR_MASK = QTD_STATUS_HALTED | QTD_STATUS_BUFFER_ERROR
                                   | QTD_STATUS_BABBLE | QTD_STATUS_TRANSACTION_ERROR;

constexpr uint32_t QTD_PID_OUT    = 0u << 8;
constexpr uint32_t QTD_PID_IN     = 1u << 8;
constexpr uint32_t QTD_PID_SETUP  = 2u << 8;
constexpr uint32_t QTD_CERR_3     = 3u << 10; // error counter: 3 retries before halting, the spec's usual choice
constexpr uint32_t QTD_DATA1      = 1u << 31;
constexpr uint32_t QTD_TERMINATE  = 1u << 0;  // "T" bit, shared by every EHCI pointer field

// Queue Element Transfer Descriptor - describes ONE transaction (SETUP,
// one packet's worth of DATA, or STATUS). 8 dwords (32 bytes) - the
// 32-bit-addressing-only layout (this project's controllers so far
// have all been well within 32-bit physical address space, so the
// 64-bit-addressing qTD extension is never needed).
struct QueueTD {
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5]; // buffer[0]'s low 12 bits are a starting offset within its page; buffer[1..4] must be page-aligned
};
static_assert(sizeof(QueueTD) == 32, "EHCI qTDs are 8 dwords");

// Queue Head - one endpoint's transfer queue. 12 dwords (48 bytes).
// This project only ever needs ONE: the default control pipe,
// reprogrammed for each new device/transfer (device address, max
// packet size, and the qTD chain all change per call) rather than
// pooled - nothing here does more than one control transfer at a time.
struct QueueHead {
    uint32_t horizontal_link;
    uint32_t endpoint_chars;
    uint32_t endpoint_caps;
    uint32_t current_qtd;
    // Transfer overlay - mirrors a QueueTD's layout exactly. The
    // controller COPIES an active qTD's fields in here while executing
    // it, and writes live status (Active/Halted, bytes remaining, etc)
    // back to THESE fields, not the original qTD in g_control_qtds[] -
    // completion must be polled here, not on the qTD structs themselves.
    uint32_t overlay_next_qtd;
    uint32_t overlay_alt_next_qtd;
    uint32_t overlay_token;
    uint32_t overlay_buffer[5];
};
static_assert(sizeof(QueueHead) == 48, "EHCI queue heads are 12 dwords");

// Verified against the EHCI 1.0 spec's Table 3-19 (Endpoint
// Characteristics: Queue Head DWord 1) directly - an earlier version of
// this file had H, DTC, and Endpoint Speed each shifted one bit too low
// (a transcription error, not a spec ambiguity), which silently left
// the queue head malformed enough that the controller enabled the
// async schedule without complaint but never actually advanced this
// QH's overlay at all. Confirmed correct now: H=bit15, DTC=bit14,
// EPS=bits13:12, Endpoint Number=bits11:8, Device Address=bits6:0.
constexpr uint32_t QH_TYPE_QH    = 0x1u << 1;  // horizontal_link's Type field (bits 2:1) - binary 01 = QH
constexpr uint32_t QH_DTC        = 1u << 14;   // Data Toggle Control - honor each qTD's own toggle bit, don't track it in the QH
constexpr uint32_t QH_H          = 1u << 15;   // Head of Reclamation List - always set, since this is the only QH there is
constexpr uint32_t QH_SPEED_HIGH = 0x2u << 12; // Endpoint Speed field (bits 13:12) - binary 10 = high-speed
constexpr uint32_t QH_MULT_1     = 0x1u << 30; // Endpoint Capabilities' Mult field (bits 31:30) - 1 transaction/microframe; 00 is RESERVED, not "0 transactions"
constexpr uint32_t QH_MAX_PACKET_SHIFT = 16;  // bits 26:16

// volatile: the controller reads AND writes these structures on its
// own, asynchronously to CPU execution - every field access here has
// to go through an actual memory access, never a value the compiler
// decides to cache in a register across loop iterations (which would
// turn control_transfer()'s completion poll into a real infinite loop
// at -O2). The same reasoning mmio.h's volatile pointers exist for.
alignas(32) inline volatile QueueHead g_control_qh = {};
alignas(32) inline volatile QueueTD g_control_qtds[3] = {}; // SETUP, DATA, STATUS
alignas(4096) inline uint8_t g_control_data_buffer[4096]; // bounce buffer - see control_transfer()'s comment
// Every buffer a qTD's buffer[0] ever points at should be one of this
// file's own static, DMA-tested locations - g_control_data_buffer already
// does this for the DATA stage, but until now the SETUP stage pointed
// straight at the caller's own `setup` argument, which in every real call
// site (usb.h's get_descriptor()/set_address()/set_configuration()) is a
// stack-local SetupPacket. The stack IS part of the same flat, identity-
// mapped low-memory region as everything else here (see boot/boot.asm -
// stack_bottom/stack_top are just another symbol in the same .bss the
// kernel itself, page tables, and every other static structure in this
// file live in), so this was never a virtual/physical-address mismatch -
// but it was still the one address in a real transfer's qTD chain that
// wasn't already a known-good, previously DMA-tested static location.
// Bouncing it through this buffer, exactly like the DATA stage already
// does, removes that as a variable entirely.
alignas(32) inline uint8_t g_control_setup_buffer[8];

// Marking the CONTROLLER's own MMIO registers uncacheable (see
// bring_up()) addresses one direction of a cache-coherency problem: the
// CPU reading/writing the device's registers. This addresses the OTHER
// direction: the device (as a PCI bus master) reading/writing THESE
// structures in normal RAM. x86 platforms are supposed to snoop PCI
// bus-master DMA against the CPU cache automatically, making this
// unnecessary in theory - but marking the MMIO BAR uncacheable produced
// zero change in an otherwise-consistent, always-identical real-hardware
// failure (see kernel.cpp's usbmsc output history), which is itself
// evidence the register reads were already accurate and the actual
// problem is elsewhere - this is the next most direct candidate: if the
// controller ever reads any of these structures straight from DRAM
// before the CPU's cached writes are flushed back, it could see a stale
// or partially-written qTD (e.g. an uninitialized buffer pointer),
// which would explain a hardware-detected fault on the very first real
// transfer specifically, not on the empty queue head (never written to
// again after the one-time setup in enable_async_schedule()). Called
// once from bring_up(), guarded so repeated calls (e.g. mount_usb()'s
// per-candidate-device retry loop) don't re-invalidate the TLB for no
// reason.
inline void mark_control_structures_uncacheable() {
    static bool done = false;
    if (done) return;
    done = true;
    mmio::mark_uncacheable(phys32(&g_control_qh), sizeof(g_control_qh));
    mmio::mark_uncacheable(phys32(&g_control_qtds[0]), sizeof(g_control_qtds));
    mmio::mark_uncacheable(phys32(g_control_data_buffer), sizeof(g_control_data_buffer));
    mmio::mark_uncacheable(phys32(g_control_setup_buffer), sizeof(g_control_setup_buffer));
    mmio::mark_uncacheable(phys32(g_periodic_frame_list), sizeof(g_periodic_frame_list));
}

inline void zero_qtd(volatile QueueTD& td) {
    td.next_qtd = 0;
    td.alt_next_qtd = 0;
    td.token = 0;
    for (int i = 0; i < 5; i++) td.buffer[i] = 0;
}

// Links the single control QH into the asynchronous schedule (as a
// circular list of one, H bit set) and enables the schedule. Must be
// called once before the first control_transfer() call. Bounded wait
// for USBSTS's Async Schedule Status to confirm the controller
// actually picked the list up, not just accepted the register write.
inline bool enable_async_schedule(Controller& controller) {
    g_control_qh.horizontal_link = phys32(&g_control_qh) | QH_TYPE_QH; // points to itself
    g_control_qh.endpoint_chars = QH_DTC | QH_H | QH_SPEED_HIGH;
    g_control_qh.endpoint_caps = QH_MULT_1;
    g_control_qh.current_qtd = 0;
    g_control_qh.overlay_next_qtd = QTD_TERMINATE;
    g_control_qh.overlay_alt_next_qtd = QTD_TERMINATE;
    g_control_qh.overlay_token = 0;

    mmio::write32(controller.op_base + ASYNCLISTADDR_OFFSET, phys32(&g_control_qh));

    // PERIODICLISTBASE is already a valid, harmless all-Terminate frame
    // list (see g_periodic_frame_list) - safe to actually enable PSE
    // alongside ASE now, matching a real, working reference driver
    // (found by diffing after 10+ real-hardware-only rounds on this
    // project's own Master Abort bug) rather than the spec-permitted but
    // apparently real-hardware-risky "async only" this file used before.
    // Nothing will actually get scheduled periodically (every frame list
    // entry terminates immediately), so this should be a pure no-op if
    // the theory is wrong, and might matter if this silicon's internal
    // frame-timing/advancement engine needs periodic running to reliably
    // execute ANY real transaction, not just poll an idle queue head.
    uint32_t cmd = mmio::read32(controller.op_base + USBCMD_OFFSET);
    mmio::write32(controller.op_base + USBCMD_OFFSET,
                  cmd | USBCMD_ASYNC_SCHEDULE_ENABLE | USBCMD_PERIODIC_SCHEDULE_ENABLE);

    uint64_t deadline = irq::g_tick_count + RESET_TIMEOUT_TICKS;
    while ((mmio::read32(controller.op_base + USBSTS_OFFSET) & USBSTS_ASYNC_SCHEDULE_STATUS) == 0) {
        if (irq::g_tick_count >= deadline) return false;
        cpu_pause();
    }
    return true;
}

// Submits one control transfer (SETUP, optional DATA, STATUS) to
// `device_address`'s default control pipe (endpoint 0), using
// `max_packet_size` (the device's bMaxPacketSize0 - 8 is the standard
// safe assumption before that's actually known), and busy-waits
// (bounded) for the hardware to finish it. enable_async_schedule() must
// already have been called once.
//
// `data`/`data_length` describe the DATA stage - pass data_length == 0
// for a transfer with no data stage at all (SET_ADDRESS,
// SET_CONFIGURATION). `data_in` gives the data stage's direction
// (ignored when data_length == 0). The STATUS stage is always the
// OPPOSITE direction of the data stage, or IN when there's no data
// stage — both directly from the USB 2.0 control transfer spec, not a
// choice this function makes.
//
// Data moves through a single static 4096-byte, page-aligned bounce
// buffer rather than the caller's own buffer directly, so a qTD's
// buffer[0] alone is always enough (no transfer here ever needs to
// cross a page boundary, so buffer[1..4] are never used) - at the cost
// of a hard 4096-byte cap on any one control transfer's data stage.
// Every request this project makes (device/configuration descriptors)
// is comfortably under that.
// Diagnostic-only: the raw overlay token and timeout/error outcome of the
// most recent control_transfer() call that didn't succeed on the first
// poll iteration. Not consulted by any control-flow in this file - purely
// so callers (usb::enumerate(), and the usbenum/usbmsc shell commands) can
// report WHY a transfer failed instead of just that it did. This matters
// specifically because real hardware has already shown failures QEMU
// testing never exercised (see kernel.cpp's usbenum/usbmsc), and each real
// -hardware test run is expensive (costs the keyboard until reboot - see
// fs.h's mount_any() comment) - richer diagnostics make each one count.
inline uint32_t g_last_transfer_token = 0;
inline bool g_last_transfer_timed_out = false;

inline bool control_transfer(Controller& controller, uint8_t device_address,
                              uint8_t max_packet_size, const uint8_t setup[8],
                              void* data, uint16_t data_length, bool data_in) {
    (void)controller; // the QH is already live in the async list (enable_async_schedule) - a transfer only ever touches it, not the controller's own registers again
    if (data_length > sizeof(g_control_data_buffer)) return false;

    if (data_length > 0 && !data_in) {
        __builtin_memcpy(g_control_data_buffer, data, data_length);
    }

    g_control_qh.endpoint_chars = QH_DTC | QH_H | QH_SPEED_HIGH
                                | ((uint32_t)max_packet_size << QH_MAX_PACKET_SHIFT)
                                | (uint32_t)device_address;

    volatile QueueTD& setup_td = g_control_qtds[0];
    volatile QueueTD& data_td = g_control_qtds[1];
    volatile QueueTD& status_td = g_control_qtds[2];

    __builtin_memcpy(g_control_setup_buffer, setup, 8);

    zero_qtd(setup_td);
    setup_td.buffer[0] = phys32(g_control_setup_buffer);
    setup_td.token = QTD_STATUS_ACTIVE | QTD_CERR_3 | QTD_PID_SETUP | (8u << 16); // setup packets are always exactly 8 bytes, DATA0
    setup_td.alt_next_qtd = QTD_TERMINATE;

    if (data_length > 0) {
        zero_qtd(data_td);
        data_td.buffer[0] = phys32(g_control_data_buffer);
        uint32_t pid = data_in ? QTD_PID_IN : QTD_PID_OUT;
        data_td.token = QTD_STATUS_ACTIVE | QTD_CERR_3 | pid | QTD_DATA1 | ((uint32_t)data_length << 16);
        data_td.alt_next_qtd = QTD_TERMINATE;
        setup_td.next_qtd = phys32(&data_td);
    } else {
        setup_td.next_qtd = phys32(&status_td);
    }

    zero_qtd(status_td);
    bool status_in = (data_length == 0) ? true : !data_in;
    status_td.token = QTD_STATUS_ACTIVE | QTD_CERR_3 | (status_in ? QTD_PID_IN : QTD_PID_OUT) | QTD_DATA1;
    status_td.next_qtd = QTD_TERMINATE;
    status_td.alt_next_qtd = QTD_TERMINATE;
    if (data_length > 0) data_td.next_qtd = phys32(&status_td);

    // Prime the queue head with the new chain from scratch - the whole
    // overlay is reset, not just the pointer, in case a previous
    // transfer left it Halted or otherwise mid-state.
    g_control_qh.overlay_next_qtd = phys32(&setup_td);
    g_control_qh.overlay_alt_next_qtd = QTD_TERMINATE;
    g_control_qh.overlay_token = 0;
    g_control_qh.current_qtd = 0;

    uint64_t deadline = irq::g_tick_count + RESET_TIMEOUT_TICKS;
    bool ok = false;
    bool timed_out = true;
    uint32_t last_token = 0;
    while (irq::g_tick_count < deadline) {
        uint32_t token = g_control_qh.overlay_token;
        uint32_t next = g_control_qh.overlay_next_qtd;
        last_token = token;
        if (token & QTD_ERROR_MASK) { ok = false; timed_out = false; break; }
        // Done only once BOTH nothing is active AND there's nothing left
        // to advance to - checking Active alone would also (wrongly)
        // trigger in the brief gap between, e.g., SETUP finishing and
        // DATA being loaded into the overlay.
        if (!(token & QTD_STATUS_ACTIVE) && (next & QTD_TERMINATE)) { ok = true; timed_out = false; break; }
        cpu_pause();
    }
    g_last_transfer_token = last_token;
    g_last_transfer_timed_out = timed_out;

    if (ok && data_length > 0 && data_in) {
        __builtin_memcpy(data, g_control_data_buffer, data_length);
    }

    return ok;
}

constexpr uint32_t QH_ENDPOINT_SHIFT = 8; // bits 11:8

// Persistent per-direction data toggle state for the bulk endpoints -
// unlike a control transfer (which always restarts at DATA0 with every
// new SETUP), a bulk endpoint's toggle must keep alternating from one
// transfer to the next for as long as the endpoint is in use, and only
// resets when the device is reset/reconfigured. reset_bulk_toggles()
// must be called once right after enumeration, before the first bulk
// transfer - a freshly configured endpoint always starts at DATA0.
inline bool g_bulk_toggle_in = false;
inline bool g_bulk_toggle_out = false;

inline void reset_bulk_toggles() {
    g_bulk_toggle_in = false;
    g_bulk_toggle_out = false;
}

// Submits one bulk transfer (a single qTD - no SETUP/STATUS stages,
// unlike control_transfer()) to `endpoint` on `device_address`, and
// busy-waits (bounded) for it to complete. Reuses the same static QH/
// qTD pool and bounce buffer control_transfer() does - nothing in this
// project ever has a control and a bulk transfer in flight at the same
// time, so there's no need for separate storage.
//
// Tracks and applies the correct data toggle automatically via
// g_bulk_toggle_in/g_bulk_toggle_out, advancing it only on success -
// a failed transfer must be retried with the SAME toggle it was
// attempted with, not the next one.
inline bool bulk_transfer(uint8_t device_address, uint8_t endpoint, uint16_t max_packet_size,
                           bool data_in, void* data, uint16_t data_length) {
    if (data_length > sizeof(g_control_data_buffer)) return false;

    bool& toggle = data_in ? g_bulk_toggle_in : g_bulk_toggle_out;

    if (data_length > 0 && !data_in) {
        __builtin_memcpy(g_control_data_buffer, data, data_length);
    }

    g_control_qh.endpoint_chars = QH_DTC | QH_H | QH_SPEED_HIGH
                                | ((uint32_t)max_packet_size << QH_MAX_PACKET_SHIFT)
                                | ((uint32_t)(endpoint & 0xF) << QH_ENDPOINT_SHIFT)
                                | (uint32_t)device_address;

    volatile QueueTD& td = g_control_qtds[0];
    zero_qtd(td);
    td.buffer[0] = phys32(g_control_data_buffer);
    uint32_t pid = data_in ? QTD_PID_IN : QTD_PID_OUT;
    uint32_t dt = toggle ? QTD_DATA1 : 0u;
    td.token = QTD_STATUS_ACTIVE | QTD_CERR_3 | pid | dt | ((uint32_t)data_length << 16);
    td.next_qtd = QTD_TERMINATE;
    td.alt_next_qtd = QTD_TERMINATE;

    g_control_qh.overlay_next_qtd = phys32(&td);
    g_control_qh.overlay_alt_next_qtd = QTD_TERMINATE;
    g_control_qh.overlay_token = 0;
    g_control_qh.current_qtd = 0;

    uint64_t deadline = irq::g_tick_count + RESET_TIMEOUT_TICKS;
    bool ok = false;
    while (irq::g_tick_count < deadline) {
        uint32_t token = g_control_qh.overlay_token;
        uint32_t next = g_control_qh.overlay_next_qtd;
        if (token & QTD_ERROR_MASK) { ok = false; break; }
        if (!(token & QTD_STATUS_ACTIVE) && (next & QTD_TERMINATE)) { ok = true; break; }
        cpu_pause();
    }

    if (ok) {
        toggle = !toggle;
        if (data_in && data_length > 0) {
            __builtin_memcpy(data, g_control_data_buffer, data_length);
        }
    }

    return ok;
}

} // namespace ehci
