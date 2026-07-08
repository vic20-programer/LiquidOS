// usb.h — USB protocol layer: standard descriptors, setup packets, and
// full device enumeration, built entirely on top of
// ehci::control_transfer() — this file never touches an EHCI register
// directly itself.
//
// This is milestone 4 of the planned USB mass storage driver (see the
// EHCI-bring-up milestone's README): given a port that came back
// "enabled" (a real, live, high-speed device under the controller's
// control), enumerate() runs the standard USB sequence every device
// goes through before it's usable — GET_DESCRIPTOR (device, partial),
// SET_ADDRESS, GET_DESCRIPTOR (device, full), GET_DESCRIPTOR
// (configuration, with its interface/endpoint sub-descriptors), and
// SET_CONFIGURATION — and reports back what it learned: vendor/product
// ID, device class, and (critically for the NEXT milestone) which
// endpoint numbers are the bulk IN/OUT pair a mass storage device would
// use to actually move data.
//
// Still doesn't move any data through those bulk endpoints, or
// interpret the device as a mass storage device at all beyond noting
// its class code — that's Mass Storage Bulk-Only Transport, the next
// milestone's job.

#pragma once
#include <stdint.h>
#include "ehci.h"

namespace usb {

// USB 2.0 spec, table 9-2 (bmRequestType) and table 9-4 (bRequest).
constexpr uint8_t REQTYPE_DEVICE_TO_HOST = 0x80;
constexpr uint8_t REQTYPE_HOST_TO_DEVICE = 0x00;

constexpr uint8_t REQ_GET_DESCRIPTOR    = 0x06;
constexpr uint8_t REQ_SET_ADDRESS       = 0x05;
constexpr uint8_t REQ_SET_CONFIGURATION = 0x09;

constexpr uint8_t DESC_TYPE_DEVICE        = 0x01;
constexpr uint8_t DESC_TYPE_CONFIGURATION = 0x02;
constexpr uint8_t DESC_TYPE_INTERFACE     = 0x04;
constexpr uint8_t DESC_TYPE_ENDPOINT      = 0x05;

constexpr uint8_t USB_CLASS_MASS_STORAGE = 0x08;

// A USB setup packet is always exactly 8 bytes, in this exact field
// order and size - this struct's layout IS the wire format, not just a
// convenient grouping, hence __attribute__((packed)) even though every
// field here happens to already fall on a naturally-aligned offset.
struct __attribute__((packed)) SetupPacket {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};
static_assert(sizeof(SetupPacket) == 8, "USB setup packets are always exactly 8 bytes");

struct __attribute__((packed)) DeviceDescriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_index;
    uint8_t product_index;
    uint8_t serial_number_index;
    uint8_t num_configurations;
};
static_assert(sizeof(DeviceDescriptor) == 18, "USB device descriptors are always 18 bytes");

struct __attribute__((packed)) ConfigDescriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length;
    uint8_t num_interfaces;
    uint8_t configuration_value;
    uint8_t configuration_index;
    uint8_t attributes;
    uint8_t max_power;
};
static_assert(sizeof(ConfigDescriptor) == 9, "USB configuration descriptors are always 9 bytes before their sub-descriptors");

struct __attribute__((packed)) InterfaceDescriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t num_endpoints;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_index;
};
static_assert(sizeof(InterfaceDescriptor) == 9, "USB interface descriptors are always 9 bytes");

struct __attribute__((packed)) EndpointDescriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t endpoint_address; // bit 7: 1 = IN, 0 = OUT; bits 3:0 = endpoint number
    uint8_t attributes;       // bits 1:0: 00=control, 01=isochronous, 10=bulk, 11=interrupt
    uint16_t max_packet_size;
    uint8_t interval;
};
static_assert(sizeof(EndpointDescriptor) == 7, "USB endpoint descriptors are always 7 bytes");

constexpr uint8_t ENDPOINT_DIR_IN = 0x80;
constexpr uint8_t ENDPOINT_TYPE_MASK = 0x03;
constexpr uint8_t ENDPOINT_TYPE_BULK = 0x02;

// Largest configuration descriptor (including all its interface/
// endpoint sub-descriptors) this project will parse - comfortably
// larger than any simple mass-storage device's, and still well under
// ehci::control_transfer()'s 4096-byte bounce-buffer cap.
constexpr uint16_t MAX_CONFIG_DESCRIPTOR_SIZE = 256;

// What enumerate() learned about a device - just enough for a later
// milestone to start talking to its mass-storage interface directly,
// without needing to re-parse descriptors itself.
struct DeviceInfo {
    uint8_t address;
    uint8_t max_packet_size0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    bool has_bulk_in;
    uint8_t bulk_in_endpoint;
    uint16_t bulk_in_max_packet;
    bool has_bulk_out;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_out_max_packet;
};

inline bool get_descriptor(ehci::Controller& controller, uint8_t address, uint8_t max_packet_size,
                            uint8_t desc_type, uint8_t desc_index, void* buf, uint16_t length) {
    SetupPacket setup{};
    setup.request_type = REQTYPE_DEVICE_TO_HOST;
    setup.request = REQ_GET_DESCRIPTOR;
    setup.value = (uint16_t)((desc_type << 8) | desc_index);
    setup.index = 0;
    setup.length = length;

    return ehci::control_transfer(controller, address, max_packet_size,
                                   reinterpret_cast<const uint8_t*>(&setup),
                                   buf, length, /*data_in=*/true);
}

inline bool set_address(ehci::Controller& controller, uint8_t new_address) {
    SetupPacket setup{};
    setup.request_type = REQTYPE_HOST_TO_DEVICE;
    setup.request = REQ_SET_ADDRESS;
    setup.value = new_address;
    setup.index = 0;
    setup.length = 0;

    // Address 0 is used for this one call (a device is only ever
    // addressed as 0 before SET_ADDRESS assigns it a real one), with
    // the safe default max packet size of 8 - the real value isn't
    // read until the following GET_DESCRIPTOR.
    return ehci::control_transfer(controller, 0, 8,
                                   reinterpret_cast<const uint8_t*>(&setup),
                                   nullptr, 0, /*data_in=*/false);
}

inline bool set_configuration(ehci::Controller& controller, uint8_t address,
                               uint8_t max_packet_size, uint8_t configuration_value) {
    SetupPacket setup{};
    setup.request_type = REQTYPE_HOST_TO_DEVICE;
    setup.request = REQ_SET_CONFIGURATION;
    setup.value = configuration_value;
    setup.index = 0;
    setup.length = 0;

    return ehci::control_transfer(controller, address, max_packet_size,
                                   reinterpret_cast<const uint8_t*>(&setup),
                                   nullptr, 0, /*data_in=*/false);
}

// Runs the standard USB enumeration sequence against a device on a port
// ehci::reset_port() already reported as `enabled`. `new_address` is
// the address to assign it (this project only ever enumerates one
// device at a time, so a fixed small number like 1 is fine - nothing
// here tracks which addresses are already in use).
//
// Only the FIRST configuration and FIRST interface are inspected;
// their endpoint descriptors are scanned for a bulk IN and/or bulk OUT
// endpoint, which is what `out` reports back - enough for a later
// milestone to start Mass Storage Bulk-Only Transport without
// re-parsing anything itself. Returns false if any control transfer
// along the way fails.
// Diagnostic-only: which step of enumerate()'s sequence the most recent
// failed call stopped at - paired with ehci::g_last_transfer_token/
// g_last_transfer_timed_out (set by the control_transfer() call that step
// made), this is what turns "Enumeration failed" from a single opaque
// outcome into something worth reporting after an expensive real-hardware
// test. Not consulted by any control-flow here.
inline const char* g_last_enum_step = nullptr;

inline bool enumerate(ehci::Controller& controller, uint8_t new_address, DeviceInfo* out) {
    // Step 1: read just the first 8 bytes of the device descriptor at
    // the default address (0) with the safe default max packet size
    // (8) - bMaxPacketSize0, needed for every later transfer, is
    // within those first 8 bytes.
    DeviceDescriptor dev_desc{};
    if (!get_descriptor(controller, 0, 8, DESC_TYPE_DEVICE, 0, &dev_desc, 8)) {
        g_last_enum_step = "GET_DESCRIPTOR(device, partial 8 bytes) at address 0";
        return false;
    }

    uint8_t max_packet_size0 = dev_desc.max_packet_size0;

    // Step 2: assign a real address - the device stops responding at
    // address 0 after this succeeds.
    if (!set_address(controller, new_address)) {
        g_last_enum_step = "SET_ADDRESS";
        return false;
    }

    // Step 3: read the FULL device descriptor at the new address, now
    // that the real max packet size is known.
    if (!get_descriptor(controller, new_address, max_packet_size0, DESC_TYPE_DEVICE, 0, &dev_desc, sizeof(dev_desc))) {
        g_last_enum_step = "GET_DESCRIPTOR(device, full) at the new address";
        return false;
    }

    // Step 4: read the configuration descriptor in two passes - first
    // just its own 9 bytes, to learn wTotalLength (how much its
    // interface/endpoint sub-descriptors add up to), then the whole
    // thing in one shot.
    ConfigDescriptor config_desc{};
    if (!get_descriptor(controller, new_address, max_packet_size0, DESC_TYPE_CONFIGURATION, 0, &config_desc, sizeof(config_desc))) {
        g_last_enum_step = "GET_DESCRIPTOR(configuration, partial 9 bytes)";
        return false;
    }

    uint16_t total_length = config_desc.total_length;
    if (total_length > MAX_CONFIG_DESCRIPTOR_SIZE) total_length = MAX_CONFIG_DESCRIPTOR_SIZE;

    uint8_t full_config[MAX_CONFIG_DESCRIPTOR_SIZE];
    if (!get_descriptor(controller, new_address, max_packet_size0, DESC_TYPE_CONFIGURATION, 0, full_config, total_length)) {
        g_last_enum_step = "GET_DESCRIPTOR(configuration, full)";
        return false;
    }

    // Step 5: commit to this configuration before using it - a device
    // stays unconfigured (and its non-default endpoints unusable) until
    // this succeeds.
    if (!set_configuration(controller, new_address, max_packet_size0, config_desc.configuration_value)) {
        g_last_enum_step = "SET_CONFIGURATION";
        return false;
    }

    out->address = new_address;
    out->max_packet_size0 = max_packet_size0;
    out->vendor_id = dev_desc.vendor_id;
    out->product_id = dev_desc.product_id;
    out->device_class = dev_desc.device_class;
    out->interface_class = 0;
    out->interface_subclass = 0;
    out->interface_protocol = 0;
    out->has_bulk_in = false;
    out->has_bulk_out = false;

    // Step 6: walk the FIRST interface's sub-descriptors (a simple mass
    // storage device only ever has one) looking for its bulk endpoints.
    // Descriptors are packed back-to-back, each self-describing its own
    // length in its first byte - the only way to walk this list at all,
    // since there's no fixed stride between entries of different types.
    uint16_t offset = config_desc.length; // skip the configuration descriptor itself
    bool found_interface = false;

    while (offset + 2 <= total_length) {
        uint8_t desc_length = full_config[offset];
        uint8_t desc_type = full_config[offset + 1];
        if (desc_length == 0 || offset + desc_length > total_length) break; // malformed - stop rather than read garbage

        if (desc_type == DESC_TYPE_INTERFACE && !found_interface) {
            const InterfaceDescriptor* iface = reinterpret_cast<const InterfaceDescriptor*>(&full_config[offset]);
            out->interface_class = iface->interface_class;
            out->interface_subclass = iface->interface_subclass;
            out->interface_protocol = iface->interface_protocol;
            found_interface = true;
        } else if (desc_type == DESC_TYPE_ENDPOINT && found_interface) {
            const EndpointDescriptor* ep = reinterpret_cast<const EndpointDescriptor*>(&full_config[offset]);
            if ((ep->attributes & ENDPOINT_TYPE_MASK) == ENDPOINT_TYPE_BULK) {
                bool is_in = (ep->endpoint_address & ENDPOINT_DIR_IN) != 0;
                uint8_t number = ep->endpoint_address & 0x0F;
                if (is_in && !out->has_bulk_in) {
                    out->has_bulk_in = true;
                    out->bulk_in_endpoint = number;
                    out->bulk_in_max_packet = ep->max_packet_size;
                } else if (!is_in && !out->has_bulk_out) {
                    out->has_bulk_out = true;
                    out->bulk_out_endpoint = number;
                    out->bulk_out_max_packet = ep->max_packet_size;
                }
            }
        }

        offset += desc_length;
    }

    return true;
}

} // namespace usb
