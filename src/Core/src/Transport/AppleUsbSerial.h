// SPDX-License-Identifier: GPL-3.0-only
//
// Apple USB serial normalization. Extracted verbatim from
// LibUsb0Transport.cpp: the rule is pure string handling with no USB backend
// involved, and both the Windows libusb0 path and the shared capture session
// have to compare serials the same way.

#pragma once

#include <string>
#include <string_view>

namespace iPhoneMirror::transport {

// Apple exposes the same physical-device serial as either a 24-character USB
// descriptor value or a 25-character usbmux UDID with a hyphen after byte 8.
// Normalization trims padding whitespace and embedded NULs, inserts the
// hyphen for the 24-character form and lowercases the result.
[[nodiscard]] std::string normalize_apple_usb_serial(std::string_view source);

// Keep readiness checks and the capture open path on one comparison rule.
[[nodiscard]] bool apple_usb_serial_equal(
    std::string_view left, std::string_view right) noexcept;

} // namespace iPhoneMirror::transport
