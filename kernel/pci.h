// pci.h — enumerates devices on the PCI bus via the legacy
// "configuration mechanism #1" (CONFIG_ADDRESS/CONFIG_DATA ports,
// 0xCF8/0xCFC) — plain port I/O, no MMIO or paging setup needed yet,
// the same complexity level as every other driver in this kernel so
// far (ata.h, keyboard.h, pit.h).
//
// This is milestone 1 of a planned USB mass storage driver (see the
// storage-device-switching milestone's README): USB host controllers
// are PCI devices whose actual runtime registers live in memory-mapped
// I/O space, so finding them - and reading their BAR (Base Address
// Register), the physical address a LATER milestone will map and
// drive - has to come before anything else can happen. This milestone
// only reads PCI CONFIGURATION space (itself accessed via plain port
// I/O); it doesn't touch MMIO, doesn't reset or program any device, and
// doesn't need paging beyond whatever the kernel already has set up.
//
// Deliberately brute-force: every (bus, device, function) in the full
// legal range is probed directly, rather than discovering buses by
// walking PCI-to-PCI bridges. A "nothing here" probe is just 2 port
// I/O operations (one CONFIG_ADDRESS write, one CONFIG_DATA read), so
// even the worst case (256 buses * 32 devices) costs on the order of
// tens of milliseconds - completely negligible for a boot-time or
// on-demand shell command, and far simpler than bridge-topology
// discovery. A later milestone can add that if something interesting
// ever turns out to live behind a bridge this misses.

#pragma once
#include <stdint.h>

namespace pci {

inline uint32_t inl(uint16_t port) {
    uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

constexpr uint16_t CONFIG_ADDRESS = 0xCF8;
constexpr uint16_t CONFIG_DATA    = 0xCFC;

constexpr uint16_t MAX_BUS      = 255; // full 8-bit bus number range
constexpr uint8_t MAX_DEVICE    = 31;
constexpr uint8_t MAX_FUNCTION  = 7;

// Reads one 32-bit dword from (bus, device, function)'s configuration
// space at `offset` (rounded down to a multiple of 4 - CONFIG_ADDRESS
// only has bits for a dword-aligned offset).
inline uint32_t read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)device << 11)
                      | ((uint32_t)function << 8) | (uint32_t)(offset & 0xFC);
    outl(CONFIG_ADDRESS, address);
    return inl(CONFIG_DATA);
}

inline uint16_t read_config_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = read_config_dword(bus, device, function, offset);
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

inline uint8_t read_config_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = read_config_dword(bus, device, function, offset);
    return (uint8_t)(dword >> ((offset & 3) * 8));
}

inline void write_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)device << 11)
                      | ((uint32_t)function << 8) | (uint32_t)(offset & 0xFC);
    outl(CONFIG_ADDRESS, address);
    outl(CONFIG_DATA, value);
}

constexpr uint16_t VENDOR_ID_NONE = 0xFFFF; // what an empty slot reads back as

// PCI class/subclass/prog-if for USB host controllers - the thing this
// whole milestone exists to find. Prog-if identifies which of the 4
// incompatible USB host controller register interfaces is in use; a
// later milestone needs to know this before it can drive one.
constexpr uint8_t CLASS_SERIAL_BUS = 0x0C;
constexpr uint8_t SUBCLASS_USB     = 0x03;
constexpr uint8_t PROGIF_UHCI = 0x00;
constexpr uint8_t PROGIF_OHCI = 0x10;
constexpr uint8_t PROGIF_EHCI = 0x20;
constexpr uint8_t PROGIF_XHCI = 0x30;

// PCI class/subclass for network controllers - the networking series'
// equivalent starting point to the USB series' CLASS_SERIAL_BUS above.
// Unlike USB host controllers, there's no small fixed set of standard
// prog-if-identified register interfaces here - real NIC hardware
// (Realtek, Broadcom, Intel, etc.) each have their own chip-specific
// register layout, identified by vendor_id/device_id rather than a
// class-level prog-if. This milestone only finds and classifies whatever
// NIC is present; a later milestone can't pick a driver strategy until
// this identifies the exact chip on the real target hardware.
constexpr uint8_t CLASS_NETWORK    = 0x02;
constexpr uint8_t SUBCLASS_ETHERNET = 0x00;

struct DeviceInfo {
    uint8_t bus, device, function;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, header_type;
    uint32_t bar[6]; // raw BAR0..BAR5 - see parse_bar() below for what these actually mean
};

inline bool read_device(uint8_t bus, uint8_t device, uint8_t function, DeviceInfo* out) {
    uint16_t vendor_id = read_config_word(bus, device, function, 0x00);
    if (vendor_id == VENDOR_ID_NONE) return false;

    out->bus = bus;
    out->device = device;
    out->function = function;
    out->vendor_id = vendor_id;
    out->device_id = read_config_word(bus, device, function, 0x02);
    out->class_code = read_config_byte(bus, device, function, 0x0B);
    out->subclass = read_config_byte(bus, device, function, 0x0A);
    out->prog_if = read_config_byte(bus, device, function, 0x09);
    out->header_type = read_config_byte(bus, device, function, 0x0E);

    for (int i = 0; i < 6; i++) {
        out->bar[i] = read_config_dword(bus, device, function, (uint8_t)(0x10 + i * 4));
    }
    return true;
}

constexpr uint16_t COMMAND_OFFSET       = 0x04; // low 16 bits of the dword at 0x04; high 16 bits are Status
constexpr uint16_t COMMAND_MEMORY_SPACE = 1u << 1; // must be set for BAR-mapped registers to respond at all
constexpr uint16_t COMMAND_BUS_MASTER   = 1u << 2; // must be set for the device to be allowed to initiate DMA

// PCI Status register - the high 16 bits of the same dword as Command
// (offset 0x04), i.e. reachable as config offset 0x06 as a standalone
// word. EHCI's own USBSTS.Host System Error bit (see ehci.h) tells you
// hardware hit A bus-level fault, but not WHICH kind - this register's
// W1C event bits distinguish them: Master Abort (nobody on the bus
// claimed the target address at all) vs Target Abort (something DID
// claim it, then explicitly rejected the transaction) vs Parity Error
// (data corruption detected). Not read anywhere in this project until
// several real-hardware-only EHCI failures in a row left USBSTS's own
// Host System Error bit as the most specific information available -
// this is strictly more specific, for the same event.
constexpr uint16_t STATUS_OFFSET               = 0x04; // read the dword, use the high 16 bits
constexpr uint16_t STATUS_SIGNALED_TARGET_ABORT = 1u << 11;
constexpr uint16_t STATUS_RECEIVED_TARGET_ABORT = 1u << 12;
constexpr uint16_t STATUS_RECEIVED_MASTER_ABORT = 1u << 13;
constexpr uint16_t STATUS_SIGNALED_SYSTEM_ERROR = 1u << 14;
constexpr uint16_t STATUS_DETECTED_PARITY_ERROR = 1u << 15;

inline uint16_t read_status(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t dword = read_config_dword(bus, device, function, (uint8_t)STATUS_OFFSET);
    return (uint16_t)(dword >> 16);
}

// Found the hard way: register-level MMIO access to a PCI device (reading/
// writing its BARs directly) works regardless of this bit, so a driver can
// reset a controller, poll its status registers, and appear to be making
// progress with Bus Master Enable still clear - but the device's own DMA
// reads/writes of system RAM (which is exactly how EHCI fetches its queue
// heads/qTDs to execute a transfer) are silently dropped by the chipset
// until this is set. QEMU's device emulation doesn't enforce this bit at
// all, so code that forgets it works perfectly under QEMU and then hangs
// forever on real hardware waiting for a DMA-driven register to change -
// see ehci.h's control_transfer(), whose completion poll never advances
// because the controller can reset/start/report status registers fine but
// can never actually fetch the queue head to run it. Real BIOSes commonly
// leave this bit clear for devices they don't themselves drive at boot,
// unlike QEMU's default device state.
inline void enable_bus_mastering(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t dword = read_config_dword(bus, device, function, (uint8_t)COMMAND_OFFSET);
    uint16_t command = (uint16_t)(dword & 0xFFFFu);
    command |= COMMAND_BUS_MASTER | COMMAND_MEMORY_SPACE;
    uint32_t new_dword = (dword & 0xFFFF0000u) | command;
    write_config_dword(bus, device, function, (uint8_t)COMMAND_OFFSET, new_dword);
}

// What a parsed/sized BAR (Base Address Register) actually means -
// PCI overloads a BAR's low bits as type flags, not just address bits.
struct BarInfo {
    bool valid;         // false if this BAR isn't implemented at all (reads as 0 both before and after sizing)
    bool is_io;         // true: I/O space (access via in/out to a port number); false: memory-mapped
    bool is_64bit;       // only meaningful when !is_io - this BAR and the next form one 64-bit address
    bool prefetchable;   // only meaningful when !is_io
    uint64_t base;       // physical base address (or I/O port number, if is_io)
    uint32_t size;       // region size in bytes (I/O BARs: in address-space units, effectively bytes too)
};

// Determines a BAR's type (I/O vs memory, 32- vs 64-bit) and size, using
// the standard PCI technique: write all-1s to the BAR register, read
// back what stuck (a device hardwires the low, "don't care" address
// bits to 0, so the readback's cleared bits reveal exactly how much
// address space it decodes), then restore the original value. A BAR
// that's entirely unimplemented reads back as 0 before AND after this -
// `valid` is false in that case, `base`/`size` are meaningless.
//
// This has to run BEFORE anything tries to use a BAR's address for
// real: an all-zero BAR doesn't necessarily mean "there's nothing
// here", it can also mean "implemented, but the BIOS never assigned it
// an address" - sizing still works either way, since it only depends on
// the device responding to the probe, not on a valid base already being
// present.
inline BarInfo parse_bar(uint8_t bus, uint8_t device, uint8_t function, int bar_index) {
    BarInfo info{};
    uint8_t offset = (uint8_t)(0x10 + bar_index * 4);

    uint32_t original = read_config_dword(bus, device, function, offset);
    info.is_io = (original & 0x1) != 0;

    if (info.is_io) {
        write_config_dword(bus, device, function, offset, 0xFFFFFFFF);
        uint32_t readback = read_config_dword(bus, device, function, offset);
        write_config_dword(bus, device, function, offset, original); // restore - never leave hardware mid-probe

        uint32_t size_mask = readback & 0xFFFFFFFC; // bits 0-1 are reserved/type, not address
        info.size = (size_mask == 0) ? 0 : (~size_mask + 1);
        info.base = original & 0xFFFFFFFC;
        info.valid = (info.size != 0);
        return info;
    }

    uint8_t type = (uint8_t)((original >> 1) & 0x3); // 0 = 32-bit, 2 = 64-bit (1 and 3 are reserved)
    info.is_64bit = (type == 2);
    info.prefetchable = (original & 0x8) != 0;
    info.base = original & 0xFFFFFFF0;

    if (!info.is_64bit) {
        write_config_dword(bus, device, function, offset, 0xFFFFFFFF);
        uint32_t readback = read_config_dword(bus, device, function, offset);
        write_config_dword(bus, device, function, offset, original);

        uint32_t size_mask = readback & 0xFFFFFFF0; // bits 0-3 are type/prefetch flags, not address
        info.size = (size_mask == 0) ? 0 : (~size_mask + 1);
        info.valid = (info.size != 0);
        return info;
    }

    // 64-bit BAR: this slot holds bits 31:0 of the base, the NEXT slot
    // holds bits 63:32 - both must be probed together.
    uint32_t upper_original = read_config_dword(bus, device, function, (uint8_t)(offset + 4));
    info.base |= ((uint64_t)upper_original << 32);

    write_config_dword(bus, device, function, offset, 0xFFFFFFFF);
    write_config_dword(bus, device, function, (uint8_t)(offset + 4), 0xFFFFFFFF);
    uint32_t readback_lo = read_config_dword(bus, device, function, offset);
    uint32_t readback_hi = read_config_dword(bus, device, function, (uint8_t)(offset + 4));
    write_config_dword(bus, device, function, offset, original);
    write_config_dword(bus, device, function, (uint8_t)(offset + 4), upper_original);

    uint64_t size_mask = ((uint64_t)readback_hi << 32) | (readback_lo & 0xFFFFFFF0);
    uint64_t size64 = (size_mask == 0) ? 0 : (~size_mask + 1);
    info.size = (uint32_t)size64; // truncated - every USB controller's register space is KBs, not GBs
    info.valid = (size64 != 0);
    return info;
}

constexpr uint32_t MAX_DEVICES = 32; // generous for a typical PC's device count

inline uint32_t g_device_count = 0;
inline DeviceInfo g_devices[MAX_DEVICES];

// Populates g_devices/g_device_count from scratch - safe to call more
// than once (e.g. `lspci` re-running it live), always starts clean.
// Function 0 of every (bus, device) is always checked; functions 1-7
// are only checked when function 0's header type says "multi-function"
// (bit 7 of the header type byte) - a single-function device (the
// common case) never has anything behind functions 1-7 to find.
inline void enumerate() {
    g_device_count = 0;

    for (uint16_t bus = 0; bus <= MAX_BUS && g_device_count < MAX_DEVICES; bus++) {
        for (uint8_t device = 0; device <= MAX_DEVICE && g_device_count < MAX_DEVICES; device++) {
            DeviceInfo info;
            if (!read_device((uint8_t)bus, device, 0, &info)) continue;
            g_devices[g_device_count++] = info;

            if ((info.header_type & 0x80) != 0) {
                for (uint8_t function = 1; function <= MAX_FUNCTION && g_device_count < MAX_DEVICES; function++) {
                    DeviceInfo finfo;
                    if (read_device((uint8_t)bus, device, function, &finfo)) {
                        g_devices[g_device_count++] = finfo;
                    }
                }
            }
        }
    }
}

} // namespace pci
