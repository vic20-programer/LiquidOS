// mmio.h — reads/writes memory-mapped device registers at a physical
// address, e.g. a PCI BAR that pci::parse_bar() found to be
// memory-mapped (an EHCI/xHCI controller's registers, typically).
//
// This only works at all because boot.asm identity-maps the full 4GiB
// 32-bit physical address space (see its p2_table_0..3 comment) — a
// physical address and its virtual address are the same number, so
// "access physical address X" is just "dereference a volatile pointer
// built from X". There is no separate MMIO address space to switch
// into or any driver-specific setup needed beyond that identity map
// already existing; this file is 3 one-line functions.
//
// `volatile` matters here for the same reason it matters for the VGA
// buffer in kernel.cpp: without it, the COMPILER is free to assume a
// memory location doesn't change behind its back and to reorder,
// or eliminate "redundant" reads/writes in the GENERATED CODE — all of
// which are actively wrong for a hardware register that changes on its
// own or has side effects on every access.
//
// `volatile` does NOT, however, do anything about the CPU's own hardware
// cache - that's a page-table-level property (the PCD bit), completely
// separate from this C++ keyword, and boot.asm's identity map leaves
// EVERY page - including wherever a PCI BAR's MMIO registers land -
// as ordinary write-back cacheable (see set_up_page_tables: `or eax,
// 0b10000011` is just Present+Writable+huge-page, no PCD). A real EHCI
// controller's registers change on their own, asynchronously to CPU
// execution (that's the entire point of polling USBSTS) - if the CPU
// caches a read of that register, every subsequent read can return the
// SAME stale cached value indefinitely, regardless of what the hardware
// register now actually holds, and a write can sit in a cache line
// without ever reaching the device. See mark_uncacheable() below - this
// is a well-documented, real-hardware-only class of bug (invisible under
// QEMU/TCG, which doesn't meaningfully emulate CPU caching at all) that
// this project hit and diagnosed the hard way; see ehci.h's bring_up().

#pragma once
#include <stdint.h>

namespace mmio {

inline uint8_t read8(uint64_t phys_addr) {
    return *reinterpret_cast<volatile uint8_t*>(phys_addr);
}

inline uint16_t read16(uint64_t phys_addr) {
    return *reinterpret_cast<volatile uint16_t*>(phys_addr);
}

inline uint32_t read32(uint64_t phys_addr) {
    return *reinterpret_cast<volatile uint32_t*>(phys_addr);
}

inline void write8(uint64_t phys_addr, uint8_t value) {
    *reinterpret_cast<volatile uint8_t*>(phys_addr) = value;
}

inline void write16(uint64_t phys_addr, uint16_t value) {
    *reinterpret_cast<volatile uint16_t*>(phys_addr) = value;
}

inline void write32(uint64_t phys_addr, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(phys_addr) = value;
}

// The 4 identity-map page tables from boot.asm, each 512 8-byte PDEs
// covering 1GiB (512 * 2MiB huge pages) - together the full 4GiB. See
// boot.asm's own comment on these. Exported specifically so this
// function can patch one at runtime, since a PCI BAR's physical address
// isn't known until PCI enumeration runs, long after boot.asm already
// built the initial (uniformly cacheable) map.
extern "C" uint64_t p2_table_0[512];
extern "C" uint64_t p2_table_1[512];
extern "C" uint64_t p2_table_2[512];
extern "C" uint64_t p2_table_3[512];

constexpr uint64_t PAGE_CACHE_DISABLE = 1ull << 4; // PCD bit - see this file's top comment

// Marks every 2MiB huge page covering [phys_addr, phys_addr+size) as
// uncacheable (PCD=1), and flushes any stale TLB entry for each one.
// Must be called before the first MMIO access to that region - see
// ehci::bring_up(), the only caller so far. A no-op safety net if
// `phys_addr` somehow falls outside the 4GiB identity map this project
// has (shouldn't happen - every PCI BAR pci::parse_bar() can report is
// itself constrained to 32-bit address space).
inline void mark_uncacheable(uint64_t phys_addr, uint64_t size) {
    uint64_t start = phys_addr & ~0x1FFFFFull;       // round down to 2MiB
    uint64_t end = (phys_addr + size + 0x1FFFFF) & ~0x1FFFFFull; // round up to 2MiB

    for (uint64_t page = start; page < end; page += 0x200000) {
        uint32_t table_index = (uint32_t)((page >> 30) & 0x3);
        uint32_t entry_index = (uint32_t)((page >> 21) & 0x1FF);

        uint64_t* table = nullptr;
        switch (table_index) {
            case 0: table = p2_table_0; break;
            case 1: table = p2_table_1; break;
            case 2: table = p2_table_2; break;
            case 3: table = p2_table_3; break;
        }
        if (!table) continue;

        table[entry_index] |= PAGE_CACHE_DISABLE;
        asm volatile("invlpg (%0)" : : "r"(page) : "memory");
    }
}

} // namespace mmio
