// SPDX-License-Identifier: GPL-3.0-only
//
// Setting the active USB configuration on Linux.
//
// This exists because udev writes bConfigurationValue back to 0 after the 0x52
// vendor request re-enumerates the device (see
// Capture/UsbReenumerationPolicy.h). Windows never needs it, so it is a new
// Linux-only file rather than an addition to the shared transport.

#pragma once

#include "Transport/QtUsbTransport.h"

#include <cstdint>
#include <string>

namespace iPhoneMirror::transport {

struct SetConfigurationResult {
    bool applied{};
    // libusb error name when the request failed, or a short explanation when the
    // device could not be located. Empty on success.
    std::string diagnostic;
};

// Re-issues SET_CONFIGURATION for the Apple device at the given bus/address.
// The handle is opened and closed around the request; no interface is claimed,
// because claiming is what the caller does afterwards once the value has been
// observed to stick.
[[nodiscard]] SetConfigurationResult set_active_configuration(
    QtUsbContext& context, std::uint8_t bus, std::uint8_t address,
    std::uint8_t configuration) noexcept;

} // namespace iPhoneMirror::transport
