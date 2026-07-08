// fs.h — LiquidFS: a minimal filesystem, mountable from any of the up
// to 4 drives ata.h can address, OR from a USB mass-storage device
// reached through ehci.h/usb.h/msc.h.
//
// This is deliberately NOT FAT, ext2, or anything with real-world
// compatibility — building a from-scratch filesystem in one milestone
// means picking the simplest layout that still teaches the real
// concepts (superblock, directory table, data region) without weeks of
// spec-reading. Layout, fixed at format time by tools/mkfs.py (and kept
// unchanged by write_file()/delete_file() at runtime):
//
//   LBA 0           Superblock: magic "LQFS", version (u32), file_count
//                    (u32), then free_extent_count (u32) and up to
//                    MAX_FREE_EXTENTS (start_lba, sector_count) pairs —
//                    all of this fits in bytes the original superblock
//                    left unused, so nothing below it moved.
//   LBA 1..4        Directory table: up to MAX_FILES entries, 64 bytes
//                    each (8 entries/sector * 4 sectors = 32 files max)
//   LBA 5..end       File data, each file starting on its own sector
//                    boundary (the directory entry stores which LBA)
//
// mount(drive)/mount_usb()/unmount() control WHICH physical device the
// rest of this file talks to — only one filesystem is ever mounted at
// a time, so "switching" means unmounting one device and mounting
// another, not multiple simultaneous mount points. g_backend records
// which underlying driver (ata:: or msc::) owns the mounted device;
// every on-disk access goes through dev_read_sector()/dev_write_sector()
// at the bottom of this file, which dispatch to the right one, so
// everything above them (directory logic, the free list, write_file(),
// ...) has no idea which physical transport it's actually running over.
//
// mount() also tries TWO different starting offsets on whichever ATA
// drive it's given (see g_partition_start_lba,
// COMBINED_IMAGE_PARTITION_LBA): LBA 0, for a disk dedicated entirely to
// LiquidFS (QEMU's disk.img/disk2.img), and a second, later offset for
// a single medium that ALSO holds a GRUB-bootable ISO region ahead of
// the LiquidFS region (the Makefile's `usb` target) — the layout needed
// to put both the OS and its data on one USB stick, since the boot
// structure has to start at LBA 0 and the two can't both live there.
// mount_usb() tries the same two offsets on whatever mass-storage
// device it finds.
//
// write_file() creates new files and overwrites existing ones at
// runtime; delete_file() removes them. Both go through a real free
// list for the data region (allocate_extent()/free_list_add() below)
// instead of only ever bump-allocating forward — the same evolution
// allocator.h went through before heap.h replaced it, here applied to
// disk sectors instead of RAM. No subdirectories. Still not a real
// on-disk format — that's future-milestone material.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ata.h"
#include "strutil.h"
#include "pci.h"
#include "mmio.h"
#include "ehci.h"
#include "usb.h"
#include "msc.h"

namespace fs {

constexpr uint32_t SUPERBLOCK_LBA = 0;
constexpr uint32_t DIR_START_LBA = 1;
constexpr uint32_t DIR_SECTORS = 4;
constexpr uint32_t DATA_START_LBA = DIR_START_LBA + DIR_SECTORS; // 5
constexpr uint32_t MAX_FILES = 32;
constexpr uint32_t NAME_LEN = 32;

struct DirEntry {
    char name[NAME_LEN];
    uint32_t start_lba;
    uint32_t size_bytes;
    uint8_t reserved[24];
};
static_assert(sizeof(DirEntry) == 64, "DirEntry must be exactly 64 bytes - mkfs.py assumes this layout");
static_assert(ata::SECTOR_SIZE % sizeof(DirEntry) == 0, "DirEntry must divide evenly into a sector");

constexpr uint32_t ENTRIES_PER_SECTOR = ata::SECTOR_SIZE / sizeof(DirEntry);

// Free-list bookkeeping, packed into the superblock sector right after
// file_count (offset 8..11): free_extent_count at offset 12, then up to
// MAX_FREE_EXTENTS (start_lba, sector_count) pairs starting at offset
// 16. 32 extents was picked to match MAX_FILES — plenty for a disk with
// at most 32 files, and the static_assert below keeps it honest as the
// sector layout evolves.
constexpr uint32_t FREE_LIST_COUNT_OFFSET = 12;
constexpr uint32_t FREE_LIST_ENTRIES_OFFSET = 16;
constexpr uint32_t MAX_FREE_EXTENTS = 32;

struct FreeExtent {
    uint32_t start_lba;
    uint32_t sector_count;
};
static_assert(FREE_LIST_ENTRIES_OFFSET + MAX_FREE_EXTENTS * sizeof(FreeExtent) <= ata::SECTOR_SIZE,
              "free list must fit in the superblock sector alongside magic/version/file_count");

// Where a combined boot+LiquidFS image (the Makefile's `usb` target)
// puts the LiquidFS region: right after a fixed-size, padded-out copy
// of the GRUB-bootable ISO. 65536 sectors = 32 MiB - comfortably larger
// than any myos.iso this project has produced so far, with headroom for
// the kernel to grow. MUST match the Makefile's BOOT_REGION_SECTORS
// exactly, or mount() will look in the wrong place on a combined image.
constexpr uint32_t COMBINED_IMAGE_PARTITION_LBA = 65536;

// Which underlying driver owns the currently mounted device. NONE
// means g_mounted is false; the two backends are mutually exclusive,
// never simultaneous - see the file header comment.
enum class Backend : uint8_t { NONE, ATA, USB_MSC };

inline bool g_mounted = false;
inline Backend g_backend = Backend::NONE;
inline uint8_t g_mounted_drive = 0; // only meaningful while g_mounted && g_backend == ATA
inline msc::Device g_usb_device{};  // only meaningful while g_mounted && g_backend == USB_MSC
// Where LiquidFS starts on the mounted device - 0 for a dedicated data
// disk, COMBINED_IMAGE_PARTITION_LBA for a combined boot+data image.
// Every on-disk access goes through dev_read_sector()/dev_write_sector()
// below, which add this offset, so SUPERBLOCK_LBA/DIR_START_LBA/etc.
// stay simple, 0-based constants regardless of which layout is active.
inline uint32_t g_partition_start_lba = 0;
inline uint32_t g_file_count = 0;
inline DirEntry g_entries[MAX_FILES];
inline uint32_t g_free_extent_count = 0;
inline FreeExtent g_free_extents[MAX_FREE_EXTENTS];

inline uint32_t mounted_drive() { return g_mounted_drive; }
inline Backend mounted_backend() { return g_backend; }

// The only two functions in this file that know there are two possible
// transports at all - everything above them (directory logic, the free
// list, write_file(), ...) just calls these and doesn't care whether
// the bytes came from a legacy IDE port or a USB bulk endpoint.
inline bool dev_read_sector(uint32_t lba, void* buf) {
    uint32_t abs_lba = g_partition_start_lba + lba;
    if (g_backend == Backend::USB_MSC) {
        return msc::read10(g_usb_device, abs_lba, 1, ata::SECTOR_SIZE, buf);
    }
    return ata::read_sector(abs_lba, buf);
}
inline bool dev_write_sector(uint32_t lba, const void* buf) {
    uint32_t abs_lba = g_partition_start_lba + lba;
    if (g_backend == Backend::USB_MSC) {
        return msc::write10(g_usb_device, abs_lba, 1, ata::SECTOR_SIZE, const_cast<void*>(buf));
    }
    return ata::write_sector(abs_lba, buf);
}

// Checks whether `drive` looks like a LiquidFS disk (correct magic in
// its superblock), at EITHER known partition offset (see
// COMBINED_IMAGE_PARTITION_LBA), WITHOUT mounting it or disturbing
// whatever's currently mounted — used by `lsdisk` to preview drives
// before committing to a mount() call. Saves and restores
// ata::g_current_drive itself (rather than trusting every caller to
// remember), since leaving it pointed at the wrong drive would make the
// NEXT read_file()/write_file() on the actually-mounted filesystem
// silently hit the wrong disk. Deliberately doesn't touch
// g_partition_start_lba at all - this is a peek, not a mount.
inline bool looks_like_liquidfs(uint8_t drive) {
    uint8_t previous_drive = ata::g_current_drive;
    ata::select_drive(drive);

    uint8_t sector[ata::SECTOR_SIZE];
    bool ok = (ata::read_sector(SUPERBLOCK_LBA, sector) &&
               sector[0] == 'L' && sector[1] == 'Q' && sector[2] == 'F' && sector[3] == 'S')
           || (ata::read_sector(COMBINED_IMAGE_PARTITION_LBA + SUPERBLOCK_LBA, sector) &&
               sector[0] == 'L' && sector[1] == 'Q' && sector[2] == 'F' && sector[3] == 'S');

    ata::select_drive(previous_drive);
    return ok;
}

// Clears the in-memory filesystem state without touching the disk —
// the counterpart to mount()/mount_usb(). After this, g_mounted is
// false and read_file()/write_file()/delete_file() all refuse to do
// anything (they check g_mounted first) until mount()/mount_usb() is
// called again. Doesn't touch which ATA drive is selected or the USB
// device's bring-up state - just this file's own bookkeeping.
inline void unmount() {
    g_mounted = false;
    g_backend = Backend::NONE;
    g_file_count = 0;
    g_free_extent_count = 0;
}

// Reads the superblock and directory table off whatever device
// g_backend/g_mounted_drive/g_usb_device CURRENTLY point at (the
// caller is responsible for pointing them at the right thing first),
// at `partition_start_lba`, into the in-memory g_entries table (and
// the free list into g_free_extents) — the whole directory is tiny (at
// most 32 * 64 = 2KB) so keeping it memory-resident avoids re-reading
// the disk for every lookup. write_file()/delete_file() keep g_entries
// and g_free_extents in sync with every change they make on disk, so
// this cache never goes stale as long as writes only ever go through
// those two functions (nothing else touches the disk's directory or
// superblock regions).
//
// A bad magic (or a read failure before it's even checked) leaves
// g_partition_start_lba restored to whatever it was and returns false
// WITHOUT touching g_mounted/g_entries/etc — trying a blank or missing
// device (or the wrong partition offset on a real one) can't corrupt
// or unmount a filesystem that's already mounted from somewhere else.
// This is the same leak/inconsistency-over-corruption preference as
// write_file()/delete_file()'s persist ordering, just applied to
// "which device (and where on it) is this kernel looking at" instead
// of "which sectors does a file own." Callers (mount(), mount_usb())
// are responsible for rolling back g_backend/drive-selection/USB
// device state themselves on failure — this function only ever owns
// g_partition_start_lba and the mounted-filesystem state.
inline bool try_mount_current_backend_at(uint32_t partition_start_lba) {
    uint32_t previous_partition_start = g_partition_start_lba;
    g_partition_start_lba = partition_start_lba;

    uint8_t sector[ata::SECTOR_SIZE];
    if (!dev_read_sector(SUPERBLOCK_LBA, sector) ||
        sector[0] != 'L' || sector[1] != 'Q' || sector[2] != 'F' || sector[3] != 'S') {
        g_partition_start_lba = previous_partition_start; // not a LiquidFS disk here - leave things as they were
        return false;
    }

    uint32_t file_count;
    __builtin_memcpy(&file_count, sector + 8, sizeof(uint32_t));
    if (file_count > MAX_FILES) file_count = MAX_FILES;
    g_file_count = file_count;

    uint32_t free_count;
    __builtin_memcpy(&free_count, sector + FREE_LIST_COUNT_OFFSET, sizeof(uint32_t));
    if (free_count > MAX_FREE_EXTENTS) free_count = MAX_FREE_EXTENTS;
    g_free_extent_count = free_count;
    // Always copies the whole fixed-size array (constant size, so GCC
    // inlines it - a size scaled by the runtime free_count instead
    // forces a real call to memcpy(), which this freestanding kernel
    // has no libc to provide). Entries past free_count are simply never
    // read by anything, same as the directory table past file_count.
    __builtin_memcpy(g_free_extents, sector + FREE_LIST_ENTRIES_OFFSET, sizeof(g_free_extents));

    uint8_t dir_sector[ata::SECTOR_SIZE];
    uint32_t loaded_sector = (uint32_t)-1; // force the first read below

    for (uint32_t i = 0; i < g_file_count; i++) {
        uint32_t sector_index = i / ENTRIES_PER_SECTOR;
        uint32_t offset_in_sector = i % ENTRIES_PER_SECTOR;

        if (sector_index != loaded_sector) {
            if (!dev_read_sector(DIR_START_LBA + sector_index, dir_sector)) return false;
            loaded_sector = sector_index;
        }

        __builtin_memcpy(&g_entries[i], dir_sector + offset_in_sector * sizeof(DirEntry), sizeof(DirEntry));
    }

    g_mounted = true;
    return true;
}

// Tries `drive` at both known partition layouts — LBA 0 (a disk
// dedicated entirely to LiquidFS) first, then
// COMBINED_IMAGE_PARTITION_LBA (a single medium holding both a
// GRUB-bootable region and a LiquidFS region after it) — so the same
// mount(drive) call works regardless of which layout is actually on
// that drive, without the caller needing to know in advance. A failed
// attempt restores whatever ATA drive was selected and whatever
// backend was mounted before, untouched — same reasoning as
// try_mount_current_backend_at()'s.
inline bool mount(uint8_t drive) {
    uint8_t previous_ata_drive = ata::g_current_drive;
    Backend previous_backend = g_backend;

    ata::select_drive(drive);
    g_backend = Backend::ATA;

    if (try_mount_current_backend_at(0) || try_mount_current_backend_at(COMBINED_IMAGE_PARTITION_LBA)) {
        g_mounted_drive = drive;
        return true;
    }

    ata::select_drive(previous_ata_drive);
    g_backend = previous_backend;
    return false;
}

// Finds a USB mass-storage device (scanning the PCI bus for an EHCI
// controller, bringing it up, resetting each connected port, and
// enumerating whatever comes back enabled - see ehci.h/usb.h) and, if
// one turns up, tries mounting LiquidFS from it at both known
// partition layouts, exactly like mount() does for an ATA drive. Tries
// every enabled device on every port of every EHCI controller found,
// not just the first, before giving up.
//
// There's no equivalent to ata::select_drive()'s cheap, instant
// "point at a different one of 4 known slots" here — finding a USB
// device at all means actually running the hardware bring-up sequence,
// which either finds a specific, already-identified device or it
// doesn't. Because of that, a failed attempt on one candidate device
// simply moves on to the next; nothing about this function's own
// search touches g_mounted/g_backend until a LiquidFS superblock is
// actually confirmed on some device, so an already-good mount (ATA or
// USB) is never at risk from a mount_usb() call that doesn't pan out.
inline bool mount_usb() {
    pci::enumerate();

    for (uint32_t i = 0; i < pci::g_device_count; i++) {
        const pci::DeviceInfo& d = pci::g_devices[i];
        if (d.class_code != pci::CLASS_SERIAL_BUS || d.subclass != pci::SUBCLASS_USB) continue;
        if (d.prog_if != pci::PROGIF_EHCI) continue;

        ehci::Controller controller;
        if (!ehci::bring_up(d, &controller)) continue;

        for (uint32_t p = 0; p < controller.num_ports; p++) {
            uint32_t portsc = ehci::read_portsc(controller.op_base, p);
            if ((portsc & ehci::PORTSC_CURRENT_CONNECT_STATUS) == 0) continue;

            ehci::PortStatus status = ehci::reset_port(controller.op_base, p);
            if (!status.enabled) continue;

            if (!ehci::enable_async_schedule(controller)) continue;

            usb::DeviceInfo info;
            if (!usb::enumerate(controller, 1, &info)) continue;
            if (!info.has_bulk_in || !info.has_bulk_out) continue;
            if (info.interface_class != usb::USB_CLASS_MASS_STORAGE) continue;

            ehci::reset_bulk_toggles(); // a freshly configured endpoint always starts at DATA0

            msc::Device candidate{};
            candidate.address = info.address;
            candidate.bulk_in_endpoint = info.bulk_in_endpoint;
            candidate.bulk_in_max_packet = info.bulk_in_max_packet;
            candidate.bulk_out_endpoint = info.bulk_out_endpoint;
            candidate.bulk_out_max_packet = info.bulk_out_max_packet;
            candidate.next_tag = 1;

            Backend previous_backend = g_backend;
            msc::Device previous_usb_device = g_usb_device;

            g_backend = Backend::USB_MSC;
            g_usb_device = candidate;

            if (try_mount_current_backend_at(0) || try_mount_current_backend_at(COMBINED_IMAGE_PARTITION_LBA)) {
                return true;
            }

            g_backend = previous_backend;
            g_usb_device = previous_usb_device;
        }
    }

    return false;
}

// Tries every one of ata.h's 4 addressable drives and mounts the first
// one that turns out to have a valid LiquidFS superblock (at either
// partition layout mount() checks).
// Boot used to always call mount(ata::PRIMARY_MASTER) instead — fine on
// QEMU, where the Makefile always attaches the data disk there, but a
// bad assumption on real hardware: there's no guarantee the drive with
// LiquidFS on it is in that exact slot (or that legacy IDE port 0x1F0
// corresponds to anything real at all, e.g. when booting from USB).
// identify() filters out ABSENT/ATAPI_OR_OTHER drives cheaply before
// ever attempting a real mount() on them.
//
// Deliberately does NOT fall back to mount_usb() here, even though
// mount_usb() exists and works (see `mount usb`) — found the hard way:
// an earlier version of this function tried mount_usb() automatically
// whenever no ATA drive had LiquidFS, and on real hardware that meant
// EVERY boot with no matching ATA disk silently ran full EHCI bring-up
// (BIOS-to-OS ownership handoff, then a hard reset on every connected
// port) before the shell was ever reachable. If the machine's own
// keyboard is USB-attached — common, even on hardware old enough to
// also have PS/2 — the BIOS's USB Legacy Support stops servicing it the
// moment ownership handoff completes, and this kernel has no USB HID
// driver of its own to take over. Result: a keyboard that stops
// responding, silently, with no on-screen indication anything happened,
// recoverable only by a full reboot (which re-runs the BIOS's own USB
// Legacy Support init from scratch). That's an unacceptable thing for
// an *automatic*, every-boot code path to risk. `mount usb` (and
// mount_usb() itself) still exist and still work exactly as before —
// this is only about what happens with no explicit request.
inline bool mount_any() {
    for (uint8_t drive = 0; drive < ata::MAX_DRIVES; drive++) {
        if (ata::identify(drive, nullptr) == ata::DriveKind::ATA && mount(drive)) {
            return true;
        }
    }
    return false;
}

inline uint32_t file_count() { return g_file_count; }

// Returns nullptr if index is out of range.
inline const DirEntry* entry_at(uint32_t index) {
    if (index >= g_file_count) return nullptr;
    return &g_entries[index];
}

inline const DirEntry* find(const char* name) {
    for (uint32_t i = 0; i < g_file_count; i++) {
        if (strutil::equals(g_entries[i].name, name)) return &g_entries[i];
    }
    return nullptr;
}

// Reads up to max_len-1 bytes of `name`'s contents into out_buf and
// null-terminates it. Returns false if the file doesn't exist. Reads
// whole sectors at a time (the only unit ATA PIO understands) even when
// the requested file is smaller than a sector, discarding the unused
// tail of the last sector read.
inline bool read_file(const char* name, char* out_buf, size_t max_len, size_t* out_size) {
    const DirEntry* e = find(name);
    if (e == nullptr) return false;

    size_t to_copy = e->size_bytes;
    if (to_copy > max_len - 1) to_copy = max_len - 1;

    uint8_t sector[ata::SECTOR_SIZE];
    size_t copied = 0;
    uint32_t lba = e->start_lba;

    while (copied < to_copy) {
        if (!dev_read_sector(lba, sector)) return false;
        size_t chunk = to_copy - copied;
        if (chunk > ata::SECTOR_SIZE) chunk = ata::SECTOR_SIZE;
        __builtin_memcpy(out_buf + copied, sector, chunk);
        copied += chunk;
        lba++;
    }

    out_buf[copied] = '\0';
    if (out_size != nullptr) *out_size = copied;
    return true;
}

inline uint32_t sectors_for(uint32_t size_bytes) {
    uint32_t sectors = (size_bytes + ata::SECTOR_SIZE - 1) / ata::SECTOR_SIZE;
    return sectors == 0 ? 1 : sectors;
}

inline uint32_t free_extent_count() { return g_free_extent_count; }

inline uint32_t free_sector_count() {
    uint32_t total = 0;
    for (uint32_t i = 0; i < g_free_extent_count; i++) total += g_free_extents[i].sector_count;
    return total;
}

// "The next free LBA" beyond the end of every known file — one past the
// highest start_lba + sectors_for(size_bytes) among all files. This is
// exactly allocator.h's bump-allocator strategy, applied to disk instead
// of RAM. allocate_extent() below only falls back to this when the free
// list (see free_list_add()) has nothing big enough already, so the
// data region only ever grows when reuse genuinely isn't possible.
// Recomputed from g_entries rather than cached, since it's at most 32
// entries.
inline uint32_t next_free_lba() {
    uint32_t max_end = DATA_START_LBA;
    for (uint32_t i = 0; i < g_file_count; i++) {
        uint32_t end = g_entries[i].start_lba + sectors_for(g_entries[i].size_bytes);
        if (end > max_end) max_end = end;
    }
    return max_end;
}

inline void free_list_remove_at(uint32_t index) {
    for (uint32_t i = index; i + 1 < g_free_extent_count; i++) {
        g_free_extents[i] = g_free_extents[i + 1];
    }
    g_free_extent_count--;
}

// Adds a freed run of sectors back to the free list, coalescing with a
// physically-adjacent free extent on either side — the same idea as
// heap2's block coalescing (heap.h), just for disk sectors instead of
// RAM blocks. Since the list only ever holds already-coalesced extents,
// a newly-freed run can touch at most one extent on each side, so a
// single pass looking for "ends exactly where I start" and "starts
// exactly where I end" is enough; no repeated merging needed.
//
// If the list is already at MAX_FREE_EXTENTS and this extent doesn't
// touch an existing one, it's silently dropped — the sectors just leak,
// same as this milestone's predecessor leaked ALL freed sectors. A
// capped array staying honest about its cap beats a crash or silent
// corruption from writing past it.
inline void free_list_add(uint32_t start_lba, uint32_t sector_count) {
    int32_t before = -1, after = -1;
    for (uint32_t i = 0; i < g_free_extent_count; i++) {
        if (g_free_extents[i].start_lba + g_free_extents[i].sector_count == start_lba) before = (int32_t)i;
        if (start_lba + sector_count == g_free_extents[i].start_lba) after = (int32_t)i;
    }

    if (before >= 0 && after >= 0) {
        g_free_extents[before].sector_count += sector_count + g_free_extents[after].sector_count;
        free_list_remove_at((uint32_t)after);
    } else if (before >= 0) {
        g_free_extents[before].sector_count += sector_count;
    } else if (after >= 0) {
        g_free_extents[after].start_lba = start_lba;
        g_free_extents[after].sector_count += sector_count;
    } else if (g_free_extent_count < MAX_FREE_EXTENTS) {
        g_free_extents[g_free_extent_count].start_lba = start_lba;
        g_free_extents[g_free_extent_count].sector_count = sector_count;
        g_free_extent_count++;
    }
}

// First-fit: takes the first free extent big enough for sectors_needed.
// An exact match is removed outright; a bigger one is split, keeping the
// leftover tail on the free list — mirroring heap2::alloc()'s
// split-on-allocate. Falls back to bump-allocating past the end of the
// data region (next_free_lba()) only when nothing on the free list is
// big enough.
inline uint32_t allocate_extent(uint32_t sectors_needed) {
    for (uint32_t i = 0; i < g_free_extent_count; i++) {
        if (g_free_extents[i].sector_count >= sectors_needed) {
            uint32_t start_lba = g_free_extents[i].start_lba;
            if (g_free_extents[i].sector_count == sectors_needed) {
                free_list_remove_at(i);
            } else {
                g_free_extents[i].start_lba += sectors_needed;
                g_free_extents[i].sector_count -= sectors_needed;
            }
            return start_lba;
        }
    }
    return next_free_lba();
}

// Persists a single directory entry (g_entries[index]) to its slot on
// disk. Directory entries are packed 8-per-sector, so this is a
// read-modify-write: read the sector, overwrite just this entry's 64
// bytes, write the sector back — anything else sharing that sector
// must survive untouched.
inline bool persist_entry(uint32_t index) {
    uint32_t sector_index = index / ENTRIES_PER_SECTOR;
    uint32_t offset_in_sector = index % ENTRIES_PER_SECTOR;

    uint8_t dir_sector[ata::SECTOR_SIZE];
    if (!dev_read_sector(DIR_START_LBA + sector_index, dir_sector)) return false;
    __builtin_memcpy(dir_sector + offset_in_sector * sizeof(DirEntry), &g_entries[index], sizeof(DirEntry));
    return dev_write_sector(DIR_START_LBA + sector_index, dir_sector);
}

inline bool persist_file_count(uint32_t count) {
    uint8_t sb[ata::SECTOR_SIZE];
    if (!dev_read_sector(SUPERBLOCK_LBA, sb)) return false;
    __builtin_memcpy(sb + 8, &count, sizeof(uint32_t));
    return dev_write_sector(SUPERBLOCK_LBA, sb);
}

// Persists g_free_extent_count/g_free_extents to their reserved bytes in
// the superblock sector — a read-modify-write like persist_file_count(),
// just against a different offset in the same sector.
inline bool persist_free_list() {
    uint8_t sb[ata::SECTOR_SIZE];
    if (!dev_read_sector(SUPERBLOCK_LBA, sb)) return false;
    __builtin_memcpy(sb + FREE_LIST_COUNT_OFFSET, &g_free_extent_count, sizeof(uint32_t));
    __builtin_memcpy(sb + FREE_LIST_ENTRIES_OFFSET, g_free_extents, sizeof(g_free_extents)); // constant size - see mount()
    return dev_write_sector(SUPERBLOCK_LBA, sb);
}

// Creates `name` if it doesn't exist, or overwrites it if it does.
// Existing files whose new content still fits in their old sector
// allocation are rewritten in place; everything else (new files, or
// existing files that grew past their old allocation) draws from the
// free list first (allocate_extent()), only bump-allocating past the
// end of the data region when nothing free is big enough.
//
// allocate_extent() can MUTATE the free list (consuming or splitting an
// extent) even when this call has nothing of its own to give back — so
// free_list_dirty tracks "does the in-memory free list disagree with
// what's on disk right now", separately from reclaim_lba/reclaim_sectors
// which track "is there a newly-freed run to add". Conflating the two
// was an earlier bug here: persisting only on reclaim left a just-
// CONSUMED extent still marked free on disk, so a crash (or even just
// the next boot) could hand the same sectors to a second file and
// silently corrupt the first one.
//
// Whatever sectors the old content no longer needs — the unused tail
// when shrinking in place, or the whole old allocation when relocated
// for growth — are only added to the free list AFTER the new directory
// entry is durably persisted. A crash between "write the new data" and
// "persist the entry" leaves those sectors un-reclaimed (leaked, same
// as this milestone's predecessor leaked everything), never double-
// allocated: the alternative order (free first) could hand those same
// sectors to some other file before this entry's rewrite was safely on
// disk.
inline bool write_file(const char* name, const void* data, size_t size) {
    if (!g_mounted) return false;
    if (strutil::length(name) >= NAME_LEN) return false;

    int32_t existing_index = -1;
    for (uint32_t i = 0; i < g_file_count; i++) {
        if (strutil::equals(g_entries[i].name, name)) {
            existing_index = (int32_t)i;
            break;
        }
    }

    uint32_t needed_sectors = sectors_for((uint32_t)size);
    uint32_t index;
    uint32_t start_lba;
    bool is_new = (existing_index < 0);

    bool free_list_dirty = false;
    uint32_t reclaim_lba = 0;
    uint32_t reclaim_sectors = 0;
    bool reclaim = false;

    if (is_new) {
        if (g_file_count >= MAX_FILES) return false;
        index = g_file_count;
        start_lba = allocate_extent(needed_sectors);
        free_list_dirty = true; // may have drawn from the free list - see comment above
    } else {
        index = (uint32_t)existing_index;
        uint32_t old_lba = g_entries[index].start_lba;
        uint32_t old_sectors = sectors_for(g_entries[index].size_bytes);

        if (needed_sectors <= old_sectors) {
            start_lba = old_lba;
            if (needed_sectors < old_sectors) {
                reclaim = true;
                reclaim_lba = old_lba + needed_sectors;
                reclaim_sectors = old_sectors - needed_sectors;
                free_list_dirty = true;
            }
        } else {
            start_lba = allocate_extent(needed_sectors);
            reclaim = true;
            reclaim_lba = old_lba;
            reclaim_sectors = old_sectors;
            free_list_dirty = true;
        }
    }

    const uint8_t* src = (const uint8_t*)data;
    size_t remaining = size;
    uint8_t sector[ata::SECTOR_SIZE];

    for (uint32_t s = 0; s < needed_sectors; s++) {
        size_t chunk = remaining < ata::SECTOR_SIZE ? remaining : ata::SECTOR_SIZE;
        __builtin_memcpy(sector, src, chunk);
        if (chunk < ata::SECTOR_SIZE) {
            __builtin_memset(sector + chunk, 0, ata::SECTOR_SIZE - chunk);
        }
        if (!dev_write_sector(start_lba + s, sector)) return false;
        src += chunk;
        remaining -= chunk;
    }

    DirEntry& e = g_entries[index];
    __builtin_memset(&e, 0, sizeof(DirEntry));
    strutil::copy(e.name, name, NAME_LEN);
    e.start_lba = start_lba;
    e.size_bytes = (uint32_t)size;

    if (!persist_entry(index)) return false;

    // Written and persisted before g_file_count moves, so a failure here
    // leaves g_file_count (and therefore what's visible to ls/find) not
    // yet counting the new entry — consistent with the on-disk
    // superblock, rather than ahead of it.
    if (is_new) {
        if (!persist_file_count(g_file_count + 1)) return false;
        g_file_count++;
    }

    if (reclaim) {
        free_list_add(reclaim_lba, reclaim_sectors);
    }
    if (free_list_dirty) {
        persist_free_list(); // best-effort: on failure the sectors just leak, not corrupt
    }

    return true;
}

// Removes `name`'s directory entry entirely and returns its sectors to
// the free list. Returns false if the file doesn't exist.
//
// The directory table has no tombstones — entries are always packed
// dense in [0, g_file_count) — so deleting index `i` means shifting
// every later entry down one slot, exactly like removing an element
// from an array. Each shifted slot is persisted as it moves, and
// file_count only shrinks after every shift is durable: if a crash
// happens partway through, at worst a later file's entry is briefly
// duplicated across two disk slots (both still pointing at that file's
// real, untouched data — cosmetically odd, not corrupt). Shrinking
// file_count FIRST would be worse: a crash right after would make
// mount() stop reading the directory one entry short, silently losing
// the LAST file instead of the one actually being deleted.
//
// Only once the directory shrink is fully durable does the deleted
// file's old extent go back on the free list — same
// leak-over-corruption ordering as write_file()'s reclaim.
inline bool delete_file(const char* name) {
    if (!g_mounted) return false;

    int32_t index = -1;
    for (uint32_t i = 0; i < g_file_count; i++) {
        if (strutil::equals(g_entries[i].name, name)) {
            index = (int32_t)i;
            break;
        }
    }
    if (index < 0) return false;

    uint32_t freed_lba = g_entries[index].start_lba;
    uint32_t freed_sectors = sectors_for(g_entries[index].size_bytes);

    for (uint32_t i = (uint32_t)index; i + 1 < g_file_count; i++) {
        g_entries[i] = g_entries[i + 1];
        if (!persist_entry(i)) return false;
    }

    if (!persist_file_count(g_file_count - 1)) return false;
    g_file_count--;

    free_list_add(freed_lba, freed_sectors);
    persist_free_list(); // best-effort: on failure the sectors just leak, not corrupt

    return true;
}

} // namespace fs
