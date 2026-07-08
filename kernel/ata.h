// ata.h — PIO driver for both legacy ATA buses (primary 0x1F0-0x1F7 /
// 0x3F6, secondary 0x170-0x177 / 0x376), each with a master and a slave
// position — up to 4 addressable drives, the same "channel x position"
// layout real IDE controllers and QEMU's `-drive ...,if=ide,index=N`
// both use (index 0-3 = primary master, primary slave, secondary
// master, secondary slave, in that order).
//
// This is still the simplest possible disk driver: no DMA, no IRQ-driven
// completion — every read or write blocks the CPU polling the status
// register until the drive is ready, then transfers the 512-byte sector
// one 16-bit word at a time with `in`/`out`. Real drivers use DMA (so
// the CPU is free during the transfer) or IRQ14 (so the CPU can do
// something else while waiting for BSY to clear), but for a filesystem
// that only ever transfers a few KB at a time, PIO polling is the right
// place to start — same reasoning that made the very first keyboard
// driver poll port 0x64 before keyboard_irq.h replaced it. If LiquidOS
// later wants non-blocking disk I/O (e.g. reading a file from inside a
// task without stalling the whole kernel), THAT's the milestone to
// switch this to IRQ14 + a completion callback.
//
// EVERY busy-wait in this file is now BOUNDED (BUSY_WAIT_TIMEOUT_TICKS,
// below) after real-hardware testing found the boot-time hang this used
// to cause: fs::mount() calls read_sector() before the kernel prints a
// single character of its own banner, so an unbounded wait for a drive
// that never responds looked EXACTLY like the kernel never booted at
// all — a black screen, forever. On real hardware (as opposed to
// QEMU's emulation), reading legacy IDE ports that nothing is wired to
// commonly reads back 0xFF (open/floating bus), which has STATUS_BSY
// permanently set — precisely the case an unbounded `while (status &
// STATUS_BSY) {}` can never escape.
//
// WHICH drive read_sector()/write_sector() talk to is global, mutable
// state (g_current_drive) rather than a parameter — select_drive() sets
// it, and it stays put until something else changes it. fs.h's mount()
// is the only thing that's supposed to change it persistently; anything
// that needs to peek at a DIFFERENT drive without disturbing whatever's
// currently mounted (identify(), probe(), fs::looks_like_liquidfs())
// must save the previous value and restore it before returning — see
// those functions' comments.
//
// QEMU emulates this faithfully (it's how `-drive` / `-hdb` attached
// disks show up), so no real hardware is needed to test this.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "irq.h" // for g_tick_count - see BUSY_WAIT_TIMEOUT_TICKS below

namespace ata {

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

// Drive indices, matching QEMU's `-drive ...,if=ide,index=N` numbering
// (and real IDE's channel/position layout) exactly.
constexpr uint8_t PRIMARY_MASTER   = 0;
constexpr uint8_t PRIMARY_SLAVE    = 1;
constexpr uint8_t SECONDARY_MASTER = 2;
constexpr uint8_t SECONDARY_SLAVE  = 3;
constexpr uint8_t MAX_DRIVES       = 4;

struct BusPorts {
    uint16_t io_base;   // DATA is io_base+0, ... COMMAND/STATUS is io_base+7
    uint16_t ctrl_base; // alternate status / device control register
};

// Standard ISA port assignment for the two legacy IDE channels.
constexpr BusPorts BUS_PORTS[2] = {
    { 0x1F0, 0x3F6 }, // primary
    { 0x170, 0x376 }, // secondary
};

constexpr uint8_t STATUS_ERR = 1 << 0;
constexpr uint8_t STATUS_DRQ = 1 << 3;
constexpr uint8_t STATUS_BSY = 1 << 7;

constexpr uint8_t CMD_READ_SECTORS  = 0x20;
constexpr uint8_t CMD_WRITE_SECTORS = 0x30;
constexpr uint8_t CMD_FLUSH_CACHE   = 0xE7;
constexpr uint8_t CMD_IDENTIFY      = 0xEC;

constexpr size_t SECTOR_SIZE = 512;

// How long any single busy-wait in this file will spin before giving up
// on a drive as unresponsive. irq::g_tick_count advances at whatever
// rate pit::set_frequency() was configured for (100 Hz as of this
// milestone), so this is ~5 seconds — generous for real ATA hardware
// (which should respond in milliseconds), but short enough that a
// missing or non-existent drive fails a `mount`/`identify` call
// instead of hanging the kernel forever. Depends on interrupts being
// enabled and the PIT already ticking (true by the time kernel.cpp
// calls fs::mount() at boot, but NOT some universal guarantee — if
// g_tick_count itself were somehow stuck, so would this timeout be).
constexpr uint64_t BUSY_WAIT_TIMEOUT_TICKS = 500;

// Polls `port` until `bit` clears, or until BUSY_WAIT_TIMEOUT_TICKS
// pass — returns false on timeout instead of spinning forever.
inline bool wait_while_bit_set(uint16_t port, uint8_t bit) {
    uint64_t deadline = irq::g_tick_count + BUSY_WAIT_TIMEOUT_TICKS;
    while (inb(port) & bit) {
        if (irq::g_tick_count >= deadline) return false;
    }
    return true;
}

// Which of the 4 possible drives read_sector()/write_sector() currently
// talk to. Defaults to PRIMARY_MASTER so a kernel that never calls
// select_drive() at all behaves exactly like this driver did before
// multi-drive support existed.
inline uint8_t g_current_drive = PRIMARY_MASTER;

inline uint16_t current_io_base() { return BUS_PORTS[g_current_drive / 2].io_base; }
inline uint16_t current_ctrl_base() { return BUS_PORTS[g_current_drive / 2].ctrl_base; }
inline bool current_is_slave() { return (g_current_drive % 2) == 1; }

// Points every subsequent read_sector()/write_sector() at a different
// drive. Deliberately just a variable write — the actual hardware
// drive-select byte only gets sent lazily, next time
// select_and_address() runs, exactly like before this existed (every
// read/write already re-sent the select byte on every single call).
inline bool select_drive(uint8_t drive) {
    if (drive >= MAX_DRIVES) return false;
    g_current_drive = drive;
    return true;
}

// Reading the alternate status port and discarding the value is the
// standard ATA trick for a ~400ns delay — the spec requires waiting
// this long after selecting a drive before its status is trustworthy.
// Four reads is the conventional margin (each read takes >=100ns on
// real hardware; QEMU doesn't need it, but real ATA controllers do).
inline void io_delay_400ns() {
    uint16_t ctrl = current_ctrl_base();
    inb(ctrl);
    inb(ctrl);
    inb(ctrl);
    inb(ctrl);
}

inline bool wait_ready() {
    uint16_t status_port = current_io_base() + 7;
    // BSY must clear before any other status bit means anything. Bounded
    // — see BUSY_WAIT_TIMEOUT_TICKS — since a drive that never clears
    // BSY (or a port nothing is wired to, reading back a permanent 0xFF)
    // must fail read_sector()/write_sector() rather than hang the caller.
    if (!wait_while_bit_set(status_port, STATUS_BSY)) return false;
    uint8_t status = inb(status_port);
    if (status & STATUS_ERR) return false;
    return (status & STATUS_DRQ) != 0;
}

// Selects the CURRENT drive (g_current_drive) in LBA mode and loads the
// (lba, count=1) address registers — the setup both read_sector() and
// write_sector() need before issuing their respective command byte.
inline void select_and_address(uint32_t lba) {
    uint16_t io_base = current_io_base();
    uint8_t select_byte = (current_is_slave() ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F);
    outb(io_base + 6, select_byte); // master: 0xE0 | top4, slave: 0xF0 | top4
    io_delay_400ns();

    outb(io_base + 2, 1);
    outb(io_base + 3, (uint8_t)(lba & 0xFF));
    outb(io_base + 4, (uint8_t)((lba >> 8) & 0xFF));
    outb(io_base + 5, (uint8_t)((lba >> 16) & 0xFF));
}

// What identify() found at a drive position.
enum class DriveKind : uint8_t {
    ABSENT,          // nothing wired to this channel/position at all
    ATA,             // a plain ATA hard disk - IDENTIFY DEVICE succeeded
    ATAPI_OR_OTHER,  // channel is live, but it's not a plain ATA disk
                     // (e.g. an ATAPI CD-ROM) - nothing LiquidFS could mount
};

// Identifies what's at `drive` using the ATA spec's own IDENTIFY DEVICE
// (0xEC) command, instead of the floating-bus heuristic this used to
// rely on. Retired because the floating-bus signal turned out to be
// per-CHANNEL, not per-drive: an empty slave sharing a channel with an
// occupied master doesn't float (the channel itself is live), so the
// old check reported it present when nothing was actually there.
// IDENTIFY doesn't have that gap, because it asks the position itself
// to respond, rather than inferring from bus electrical behavior.
//
// This is the standard detection sequence (the same one real BIOSes and
// OS tutorials use):
//   0. FAST PATH: write an arbitrary test pattern to a plain
//      read/write register (sector count and LBA_LOW have no side
//      effects) and read it straight back, no waiting. A floating bus —
//      nothing wired to this position at all — can't hold a written
//      value, so a mismatch means "definitely absent" without ever
//      touching BUSY or its timeout. This matters in practice: on
//      hardware with no legacy IDE controller reachable at any of the 4
//      slots, mount_any() trying all 4 in a row used to cost 4x the
//      full BUSY_WAIT_TIMEOUT_TICKS (~20s) before this existed — this
//      check turns that into 4 near-instant rejections.
//   1. Select the drive (0xA0 master / 0xB0 slave - the LBA-mode bit
//      doesn't matter for this command), zero the count/LBA registers,
//      send IDENTIFY.
//   2. Read status immediately: 0 means NOTHING responded — this position
//      is genuinely absent. This is the check that replaces the floating-
//      bus trick, and it's per-DRIVE, not per-channel.
//   3. Otherwise, wait for BSY to clear, then check LBA_MID/LBA_HIGH:
//      non-zero means this isn't a plain ATA disk (ATAPI drives put a
//      signature there) — something answered, so the channel and this
//      position are definitely live, but stop here rather than waiting
//      for a DRQ a non-ATA device will never raise in response to this
//      command.
//   4. Otherwise poll for DRQ (success) or ERR (device didn't like the
//      command, but still responded — treated the same as
//      ATAPI_OR_OTHER: present, not usable as an ATA disk).
//   5. On DRQ, the 256-word identification block MUST be read out of the
//      data port even if the caller doesn't want it (pass out_identify
//      as nullptr) — leaving it unread leaves the drive mid-transfer,
//      which corrupts whatever command is issued to it next. This is the
//      detail that's easy to get away with skipping in QEMU but breaks
//      on real hardware.
//
// Saves and restores g_current_drive, so identifying a drive never
// disturbs whatever's actually mounted.
inline DriveKind identify(uint8_t drive, uint16_t* out_identify) {
    if (drive >= MAX_DRIVES) return DriveKind::ABSENT;

    uint8_t previous_drive = g_current_drive;
    g_current_drive = drive;
    uint16_t io_base = current_io_base();

    outb(io_base + 6, current_is_slave() ? 0xB0 : 0xA0); // select, non-LBA form
    io_delay_400ns();

    // Step 0 (see comment above): fast, non-blocking absence check.
    outb(io_base + 2, 0xAB);
    outb(io_base + 3, 0xCD);
    if (inb(io_base + 2) != 0xAB || inb(io_base + 3) != 0xCD) {
        g_current_drive = previous_drive;
        return DriveKind::ABSENT;
    }

    outb(io_base + 2, 0);
    outb(io_base + 3, 0);
    outb(io_base + 4, 0);
    outb(io_base + 5, 0);
    outb(io_base + 7, CMD_IDENTIFY);

    uint8_t status = inb(io_base + 7);
    if (status == 0) {
        g_current_drive = previous_drive;
        return DriveKind::ABSENT; // nothing at this position - confirmed per-drive
    }

    // Bounded: an undecoded port range (nothing wired to it at all) commonly
    // reads back 0xFF here, which has STATUS_BSY permanently set — exactly
    // the case status==0 above can't catch, since 0xFF != 0. Without a
    // timeout this would spin forever instead of reporting ABSENT.
    if (!wait_while_bit_set(io_base + 7, STATUS_BSY)) {
        g_current_drive = previous_drive;
        return DriveKind::ABSENT;
    }

    if (inb(io_base + 4) != 0 || inb(io_base + 5) != 0) {
        g_current_drive = previous_drive;
        return DriveKind::ATAPI_OR_OTHER; // live, but not a plain ATA disk
    }

    DriveKind result = DriveKind::ABSENT; // overwritten below unless the wait times out
    uint64_t deadline = irq::g_tick_count + BUSY_WAIT_TIMEOUT_TICKS;
    bool responded = false;
    while (irq::g_tick_count < deadline) {
        uint8_t s = inb(io_base + 7);
        if (s & STATUS_ERR) {
            result = DriveKind::ATAPI_OR_OTHER;
            responded = true;
            break;
        }
        if (s & STATUS_DRQ) {
            result = DriveKind::ATA;
            responded = true;
            break;
        }
    }
    if (!responded) {
        // Something answered earlier (status != 0, then a real LBA_MID/
        // LBA_HIGH of 0) but never raised DRQ or ERR - treat as "present,
        // not usable" rather than pretend it never responded at all.
        result = DriveKind::ATAPI_OR_OTHER;
    }

    if (result == DriveKind::ATA) {
        uint16_t discard[256];
        uint16_t* dst = (out_identify != nullptr) ? out_identify : discard;
        for (int i = 0; i < 256; i++) {
            dst[i] = inw(io_base + 0); // MUST drain all 256 words - see comment above
        }
    }

    g_current_drive = previous_drive;
    return result;
}

// Convenience wrapper over identify() for callers that only care
// whether anything at all is at `drive`, not what kind.
inline bool probe(uint8_t drive) {
    return identify(drive, nullptr) != DriveKind::ABSENT;
}

// Reads exactly one 512-byte sector at `lba` (28-bit) into `buf`, which
// must point at SECTOR_SIZE bytes, from the CURRENT drive
// (g_current_drive - see select_drive()). Returns false if the drive
// reports an error (bad LBA, no disk attached, etc).
inline bool read_sector(uint32_t lba, void* buf) {
    select_and_address(lba);
    outb(current_io_base() + 7, CMD_READ_SECTORS);

    if (!wait_ready()) return false;

    uint16_t data_port = current_io_base() + 0;
    uint16_t* dst = (uint16_t*)buf;
    for (size_t i = 0; i < SECTOR_SIZE / 2; i++) {
        dst[i] = inw(data_port);
    }
    return true;
}

// Writes exactly one 512-byte sector at `lba` (28-bit) from `buf` to the
// CURRENT drive (g_current_drive - see select_drive()), then issues
// FLUSH CACHE and waits for it to complete. The flush matters more here
// than it would on real hardware: QEMU's default disk cache mode is
// writeback, meaning a write that only reaches the drive's (emulated)
// cache is NOT yet reflected in disk.img on the host — the same file a
// `cat` after reboot, or a host-side check, would read. FLUSH CACHE is
// the standard ATA way to say "make it durable now."
inline bool write_sector(uint32_t lba, const void* buf) {
    select_and_address(lba);
    uint16_t io_base = current_io_base();
    outb(io_base + 7, CMD_WRITE_SECTORS);

    if (!wait_ready()) return false;

    const uint16_t* src = (const uint16_t*)buf;
    for (size_t i = 0; i < SECTOR_SIZE / 2; i++) {
        outw(io_base + 0, src[i]);
    }

    // The drive raises BSY again while it commits the sector; wait for
    // that to clear before trusting the write finished. Bounded, same
    // reasoning as wait_ready().
    if (!wait_while_bit_set(io_base + 7, STATUS_BSY)) return false;
    if (inb(io_base + 7) & STATUS_ERR) return false;

    outb(io_base + 7, CMD_FLUSH_CACHE);
    if (!wait_while_bit_set(io_base + 7, STATUS_BSY)) return false;
    return true;
}

} // namespace ata
