// msc.h — USB Mass Storage Class: Bulk-Only Transport (BOT) and a
// minimal SCSI command subset, built entirely on top of
// ehci::bulk_transfer() — this file never touches an EHCI register or
// the async schedule directly itself, the same layering usb.h uses for
// control transfers.
//
// This is milestone 5 of the planned USB mass storage driver (see the
// USB-device-enumeration milestone's README): given a device
// usb::enumerate() already identified as having a bulk IN/OUT pair,
// this is what finally moves real data - SCSI commands wrapped in the
// Bulk-Only Transport envelope, the same protocol every USB flash
// drive on earth speaks. New shell command: `usbmsc`.
//
// BOT is a simple three-phase exchange for every command:
//   1. Command Block Wrapper (CBW) - 31 bytes, sent OUT: which SCSI
//      command, how much data to expect, which direction.
//   2. Data phase (optional) - IN or OUT, whatever the command implies.
//   3. Command Status Wrapper (CSW) - 13 bytes, received IN: whether
//      the command succeeded, and how many bytes were NOT transferred
//      (the "residue") if it didn't move everything requested.
//
// Still doesn't wire any of this into fs.h - that's the next milestone,
// once read10()/write10() below are proven correct against a real
// virtual USB stick.

#pragma once
#include <stdint.h>
#include "ehci.h"

namespace msc {

constexpr uint32_t CBW_SIGNATURE = 0x43425355; // "USBC"
constexpr uint32_t CSW_SIGNATURE = 0x53425355; // "USBS"
constexpr uint8_t CBW_FLAGS_IN  = 0x80;
constexpr uint8_t CBW_FLAGS_OUT = 0x00;

struct __attribute__((packed)) CBW {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
};
static_assert(sizeof(CBW) == 31, "a Command Block Wrapper is always exactly 31 bytes on the wire");

struct __attribute__((packed)) CSW {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t status;
};
static_assert(sizeof(CSW) == 13, "a Command Status Wrapper is always exactly 13 bytes on the wire");

constexpr uint8_t CSW_STATUS_PASSED      = 0;
constexpr uint8_t CSW_STATUS_FAILED      = 1;
constexpr uint8_t CSW_STATUS_PHASE_ERROR = 2;

// What a later milestone needs to actually talk to a specific device -
// usb::DeviceInfo has more than this (device/vendor IDs, etc.) that
// isn't needed once you're down at the BOT/SCSI level.
struct Device {
    uint8_t address;
    uint8_t bulk_in_endpoint;
    uint16_t bulk_in_max_packet;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_out_max_packet;
    uint32_t next_tag; // arbitrary, just needs to be unique-ish per command to catch a CSW answering the wrong CBW
};

// Sends one SCSI command block wrapped in BOT's three-phase envelope,
// and validates the CSW that comes back (signature, tag echoed
// correctly, and PASSED status) - a transport-level failure at ANY
// phase (CBW, data, or CSW) or a non-PASSED SCSI status both count as
// failure here; callers that need to distinguish "transport worked but
// the device said no" from "transport itself failed" aren't
// distinguished by this milestone, since nothing here needs to yet.
inline bool send_command(Device& dev, const uint8_t* cb, uint8_t cb_length,
                          void* data, uint32_t data_length, bool data_in) {
    if (data_length > sizeof(ehci::g_control_data_buffer)) return false;

    CBW cbw{};
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = dev.next_tag++;
    cbw.data_transfer_length = data_length;
    cbw.flags = data_in ? CBW_FLAGS_IN : CBW_FLAGS_OUT;
    cbw.lun = 0;
    cbw.cb_length = cb_length;
    __builtin_memcpy(cbw.cb, cb, cb_length);

    if (!ehci::bulk_transfer(dev.address, dev.bulk_out_endpoint, dev.bulk_out_max_packet,
                              /*data_in=*/false, &cbw, sizeof(cbw))) {
        return false;
    }

    if (data_length > 0) {
        bool ok = data_in
            ? ehci::bulk_transfer(dev.address, dev.bulk_in_endpoint, dev.bulk_in_max_packet,
                                   true, data, (uint16_t)data_length)
            : ehci::bulk_transfer(dev.address, dev.bulk_out_endpoint, dev.bulk_out_max_packet,
                                   false, data, (uint16_t)data_length);
        if (!ok) return false;
    }

    CSW csw{};
    if (!ehci::bulk_transfer(dev.address, dev.bulk_in_endpoint, dev.bulk_in_max_packet,
                              true, &csw, sizeof(csw))) {
        return false;
    }

    if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag) return false;
    return csw.status == CSW_STATUS_PASSED;
}

// --- A minimal SCSI command subset -----------------------------------

constexpr uint8_t SCSI_TEST_UNIT_READY  = 0x00;
constexpr uint8_t SCSI_INQUIRY          = 0x12;
constexpr uint8_t SCSI_READ_CAPACITY_10 = 0x25;
constexpr uint8_t SCSI_READ_10          = 0x28;
constexpr uint8_t SCSI_WRITE_10         = 0x2A;

inline bool test_unit_ready(Device& dev) {
    uint8_t cb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return send_command(dev, cb, sizeof(cb), nullptr, 0, true);
}

struct __attribute__((packed)) InquiryData {
    uint8_t peripheral;
    uint8_t removable;
    uint8_t version;
    uint8_t response_format;
    uint8_t additional_length;
    uint8_t reserved[3];
    char vendor_id[8];
    char product_id[16];
    char product_revision[4];
};
static_assert(sizeof(InquiryData) == 36, "a standard INQUIRY response is 36 bytes");

inline bool inquiry(Device& dev, InquiryData* out) {
    uint8_t cb[6] = { SCSI_INQUIRY, 0, 0, 0, sizeof(InquiryData), 0 };
    return send_command(dev, cb, sizeof(cb), out, sizeof(InquiryData), true);
}

struct Capacity {
    uint32_t last_lba;   // the LAST valid LBA, not the total count - total blocks = last_lba + 1
    uint32_t block_size;
};

// SCSI multi-byte fields are big-endian on the wire, unlike everything
// else in this project (x86 is little-endian) - every SCSI command
// builder/parser in this file has to byte-swap explicitly, by hand,
// rather than just casting a buffer to a struct pointer the way
// usb.h's USB descriptors (which ARE little-endian) can.
inline bool read_capacity(Device& dev, Capacity* out) {
    uint8_t cb[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t response[8];
    if (!send_command(dev, cb, sizeof(cb), response, sizeof(response), true)) return false;

    out->last_lba = ((uint32_t)response[0] << 24) | ((uint32_t)response[1] << 16)
                   | ((uint32_t)response[2] << 8) | (uint32_t)response[3];
    out->block_size = ((uint32_t)response[4] << 24) | ((uint32_t)response[5] << 16)
                     | ((uint32_t)response[6] << 8) | (uint32_t)response[7];
    return true;
}

// Reads `count` blocks starting at `lba` into `buf`. `count *
// block_size` must fit within ehci::bulk_transfer()'s bounce-buffer
// cap (4096 bytes, see ehci.h) - no chunking of larger reads across
// multiple transfers exists yet.
inline bool read10(Device& dev, uint32_t lba, uint16_t count, uint32_t block_size, void* buf) {
    uint8_t cb[10];
    cb[0] = SCSI_READ_10;
    cb[1] = 0;
    cb[2] = (uint8_t)(lba >> 24); cb[3] = (uint8_t)(lba >> 16); cb[4] = (uint8_t)(lba >> 8); cb[5] = (uint8_t)lba;
    cb[6] = 0;
    cb[7] = (uint8_t)(count >> 8); cb[8] = (uint8_t)count;
    cb[9] = 0;
    return send_command(dev, cb, sizeof(cb), buf, (uint32_t)count * block_size, true);
}

inline bool write10(Device& dev, uint32_t lba, uint16_t count, uint32_t block_size, void* buf) {
    uint8_t cb[10];
    cb[0] = SCSI_WRITE_10;
    cb[1] = 0;
    cb[2] = (uint8_t)(lba >> 24); cb[3] = (uint8_t)(lba >> 16); cb[4] = (uint8_t)(lba >> 8); cb[5] = (uint8_t)lba;
    cb[6] = 0;
    cb[7] = (uint8_t)(count >> 8); cb[8] = (uint8_t)count;
    cb[9] = 0;
    return send_command(dev, cb, sizeof(cb), buf, (uint32_t)count * block_size, false);
}

} // namespace msc
