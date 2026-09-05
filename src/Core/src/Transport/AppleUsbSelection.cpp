// SPDX-License-Identifier: GPL-3.0-only
//
// Apple USB device selection and endpoint choice. Extracted verbatim from
// LibUsb0Transport.cpp: none of it touches a USB backend, and both the
// libusb-1.0 transport (which builds on Linux) and the Windows libusb0 filter
// backend need the same matching rules. Declarations live in QtUsbTransport.h,
// which is where every caller already looks for them.

#include "Transport/QtUsbTransport.h"

#include "Transport/AppleUsbSerial.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>

namespace iPhoneMirror::transport {

AppleUsbIdentity make_apple_usb_identity(const AppleUsbDevice& device) noexcept {
    AppleUsbIdentity identity;
    try {
        identity.serial = device.serial;
        identity.topology_id = device.topology_id;
    } catch (...) {
        return {};
    }
    identity.original_product_id = device.product_id;
    if (device.quicktime_endpoints.configuration != 0) {
        identity.expected_quicktime_configuration =
            device.quicktime_endpoints.configuration;
    } else if (device.highest_configuration_value != 0 &&
        device.highest_configuration_value != 0xff) {
        identity.expected_quicktime_configuration = static_cast<std::uint8_t>(
            device.highest_configuration_value + 1U);
    }
    return identity;
}

AppleUsbSelection select_apple_usb_device(
    std::span<const AppleUsbDevice> devices, const AppleUsbIdentity& identity,
    bool require_quicktime) noexcept {
    AppleUsbSelection result;
    std::optional<std::size_t> serial_index;
    std::optional<std::size_t> serial_topology_index;
    std::size_t serial_topology_matches{};
    std::optional<std::size_t> topology_index;

    for (std::size_t index{}; index < devices.size(); ++index) {
        const auto& device = devices[index];
        if (require_quicktime && !device.quicktime_configuration) continue;
        const bool serial_match = !identity.serial.empty() &&
            apple_usb_serial_equal(device.serial, identity.serial);
        const bool topology_match = !identity.topology_id.empty() &&
            device.topology_id == identity.topology_id;
        bool candidate_serial_available{};
        try {
            candidate_serial_available = !normalize_apple_usb_serial(device.serial).empty();
        } catch (...) {}
        if (serial_match) {
            ++result.serial_matches;
            serial_index = index;
            if (topology_match) {
                ++serial_topology_matches;
                serial_topology_index = index;
            }
        }
        // A physical-port fallback only bridges a descriptor whose serial is
        // temporarily unreadable. A known, different serial is authoritative
        // and must never be rebound to this capture session.
        if (topology_match && !candidate_serial_available) {
            ++result.topology_matches;
            topology_index = index;
        }
    }

    if (result.serial_matches == 1) {
        result.index = serial_index;
        result.match_kind = AppleUsbMatchKind::Serial;
        return result;
    }
    if (result.serial_matches > 1) {
        if (serial_topology_matches == 1) {
            result.index = serial_topology_index;
            result.match_kind = AppleUsbMatchKind::Serial;
        } else {
            result.ambiguous = true;
        }
        return result;
    }
    if (result.topology_matches == 1) {
        result.index = topology_index;
        result.match_kind = AppleUsbMatchKind::Topology;
        return result;
    }
    result.ambiguous = result.topology_matches > 1;
    return result;
}

bool apple_usb_candidate_in_scope(std::string_view candidate_topology,
    const AppleUsbIdentity& identity) noexcept {
    return !identity.serial.empty() || identity.topology_id.empty() ||
        candidate_topology == identity.topology_id;
}

UsbEndpointSet select_best_quicktime_endpoints(
    std::span<const UsbEndpointSet> candidates) noexcept {
    UsbEndpointSet selected;
    for (const auto& candidate : candidates) {
        if (candidate.configuration == 0 || candidate.bulk_in == 0 ||
            candidate.bulk_out == 0 || (candidate.bulk_in & 0x80U) == 0 ||
            (candidate.bulk_out & 0x80U) != 0) continue;
        const auto candidate_packet = (std::min)(candidate.bulk_in_packet_size,
            candidate.bulk_out_packet_size);
        const auto selected_packet = (std::min)(selected.bulk_in_packet_size,
            selected.bulk_out_packet_size);
        if (selected.configuration == 0 ||
            candidate.configuration > selected.configuration ||
            (candidate.configuration == selected.configuration &&
                candidate_packet > selected_packet)) {
            selected = candidate;
        }
    }
    return selected;
}

UsbEndpointSet conventional_quicktime_endpoints(
    const AppleUsbIdentity& identity) noexcept {
    if (identity.expected_quicktime_configuration == 0) return {};
    return {
        .configuration = identity.expected_quicktime_configuration,
        .interface_number = 2,
        .alternate_setting = 0,
        .bulk_in = 0x86,
        .bulk_out = 0x05,
        .bulk_in_packet_size = 512,
        .bulk_out_packet_size = 512,
    };
}

std::string describe_apple_usb_candidates(
    std::span<const AppleUsbDevice> devices, const AppleUsbIdentity& identity) {
    std::string description = std::format("count={}", devices.size());
    for (std::size_t index{}; index < devices.size(); ++index) {
        const auto& device = devices[index];
        description += std::format(
            " [{} vid={:04x} pid={:04x} bus={} addr={} open={} serial_match={} topology_match={} configs={}/{} qt={}:{}:{}:{:02x}:{:02x}]",
            index, device.vendor_id, device.product_id, device.bus, device.address,
            device.can_open,
            !identity.serial.empty() && apple_usb_serial_equal(device.serial, identity.serial),
            !identity.topology_id.empty() && device.topology_id == identity.topology_id,
            device.configuration_count, device.highest_configuration_value,
            device.quicktime_endpoints.configuration,
            device.quicktime_endpoints.interface_number,
            device.quicktime_endpoints.alternate_setting,
            device.quicktime_endpoints.bulk_in,
            device.quicktime_endpoints.bulk_out);
    }
    return description;
}
} // namespace iPhoneMirror::transport
