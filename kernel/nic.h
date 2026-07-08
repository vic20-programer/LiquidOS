// nic.h — milestone 2 of the networking series: parse the Ethernet
// controller's BAR and read one real register, mirroring the USB
// series' second milestone (usbbar) exactly in scope. Confirmed via
// `lspci net` on the actual ProBook 6450b (not guessed): vendor=8086
// device=10ea, an Intel 82577LM Gigabit Network Connection - PCIe,
// driven on real OSes by the e1000e family driver. QEMU's own `e1000`
// device model is the OLDER, I/O-BAR-capable 8254x family, so this
// file only relies on register-map facts that have stayed compatible
// across the whole e1000/e1000e lineage (CTRL at 0x0, STATUS at 0x8) -
// nothing 82577-specific has been assumed or could yet be verified.
//
// Deliberately does NOT reset, start, or otherwise program the
// controller - same reasoning as usbbar: confirm BAR parsing + basic
// MMIO access works before writing any register that changes hardware
// state. The two hard-won lessons from the EHCI saga (see
// liquidos_realhw_testing memory) are applied from the start rather
// than re-discovered the slow way: enable PCI Bus Master before any
// MMIO access is trusted, and mark the BAR's pages uncacheable before
// the first read, since a real e1000e's STATUS register (like EHCI's
// USBSTS) changes asynchronously to CPU execution and a stale cached
// read would look identical to "nothing's wrong" right up until it
// silently isn't.
//
// Real-hardware-confirmed (ProBook 6450b) since this file was first
// written: CTRL/STATUS reads came back sane (not garbage/0xFFFFFFFF,
// unlike the EHCI Master Abort saga) - strong evidence the bus-
// mastering/uncaching fixes above are genuine hardware lessons, not
// one-off EHCI workarounds. STATUS.PHYRA (PHY Reset Asserted, bit 10)
// was set, which fully explains the observed link-down: this milestone
// never asserts Set-Link-Up or resets the controller, so the PHY has
// never been brought up. Added next: reading RAL0/RAH0 (this NIC's own
// MAC address, loaded by hardware from EEPROM at power-on) - still
// read-only, zero risk to hardware state, ahead of the bigger, actually
// state-changing reset/link-bring-up step that comes after this.

#pragma once
#include <stdint.h>
#include "pci.h"
#include "mmio.h"

namespace nic {

constexpr uint32_t REG_CTRL   = 0x00000; // Device Control
constexpr uint32_t REG_STATUS = 0x00008; // Device Status

constexpr uint32_t STATUS_FULL_DUPLEX = 1u << 0;
constexpr uint32_t STATUS_LINK_UP     = 1u << 1;
// bits 6:7 - speed, only meaningful once STATUS_LINK_UP is set
constexpr uint32_t STATUS_SPEED_MASK  = 0x3u << 6;
constexpr uint32_t STATUS_SPEED_10    = 0x0u << 6;
constexpr uint32_t STATUS_SPEED_100   = 0x1u << 6;
constexpr uint32_t STATUS_SPEED_1000  = 0x2u << 6; // also 0x3, per spec both encode 1000Mb/s
constexpr uint32_t STATUS_PHYRA       = 1u << 10; // PHY Reset Asserted - explains link-down on an unstarted controller

// Receive Address slot 0 - the first of 16 (RAL/RAH array, only slot 0
// used here). Holds this NIC's own MAC address, loaded from the EEPROM
// by hardware at power-on, long before any driver runs - read-only for
// this milestone's purposes, verified against Linux's e1000e defines.h/
// regs.h (RAL(0)=0x5400, RAH(0)=0x5404, stable across the whole e1000/
// e1000e family) rather than assumed, since a WebFetch of OSDev's own
// Intel_8254x page (403, same block hit earlier in this project) wasn't
// available to cross-check directly.
constexpr uint32_t REG_RAL0 = 0x5400; // low 32 bits: MAC bytes 0-3, byte0 in bits 7:0
constexpr uint32_t REG_RAH0 = 0x5404; // low 16 bits: MAC bytes 4-5, byte4 in bits 7:0
constexpr uint32_t RAH_AV   = 1u << 31; // Address Valid - this slot actually holds a real address

struct ProbeResult {
    bool found;
    pci::DeviceInfo dev;
    bool have_bar;
    int bar_index;
    pci::BarInfo bar;
    bool bar_is_mmio; // false = BAR was I/O-mapped, which this milestone can't read (no chip needs that yet)
    uint32_t ctrl;
    uint32_t status;
    bool mac_valid;
    uint8_t mac[6];
};

// Finds the first PCI Ethernet controller, parses its first valid
// memory-mapped BAR, and (only if that succeeded) enables bus
// mastering, marks that BAR's pages uncacheable, and reads CTRL/STATUS.
// Safe to call repeatedly (e.g. from a shell command re-run) - always
// starts from a fresh pci::enumerate().
inline ProbeResult probe() {
    ProbeResult result{};
    pci::enumerate();

    for (uint32_t i = 0; i < pci::g_device_count; i++) {
        const pci::DeviceInfo& d = pci::g_devices[i];
        if (d.class_code != pci::CLASS_NETWORK || d.subclass != pci::SUBCLASS_ETHERNET) continue;

        result.found = true;
        result.dev = d;

        for (int b = 0; b < 6; b++) {
            pci::BarInfo bar = pci::parse_bar(d.bus, d.device, d.function, b);
            if (!bar.valid) continue;
            result.have_bar = true;
            result.bar_index = b;
            result.bar = bar;
            break; // first valid BAR only - e1000e's registers live entirely in BAR0
        }

        if (result.have_bar && !result.bar.is_io) {
            result.bar_is_mmio = true;
            pci::enable_bus_mastering(d.bus, d.device, d.function);
            mmio::mark_uncacheable(result.bar.base, result.bar.size);
            result.ctrl = mmio::read32(result.bar.base + REG_CTRL);
            result.status = mmio::read32(result.bar.base + REG_STATUS);

            uint32_t ral = mmio::read32(result.bar.base + REG_RAL0);
            uint32_t rah = mmio::read32(result.bar.base + REG_RAH0);
            result.mac_valid = (rah & RAH_AV) != 0;
            result.mac[0] = (uint8_t)(ral >> 0);
            result.mac[1] = (uint8_t)(ral >> 8);
            result.mac[2] = (uint8_t)(ral >> 16);
            result.mac[3] = (uint8_t)(ral >> 24);
            result.mac[4] = (uint8_t)(rah >> 0);
            result.mac[5] = (uint8_t)(rah >> 8);
        }

        return result; // first Ethernet controller only, same as usbprobe's first-USB-controller scope
    }

    return result;
}

} // namespace nic
