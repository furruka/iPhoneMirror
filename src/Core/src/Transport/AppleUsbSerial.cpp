// SPDX-License-Identifier: GPL-3.0-only

#include "Transport/AppleUsbSerial.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace iPhoneMirror::transport {

std::string normalize_apple_usb_serial(std::string_view source) {
    std::string value(source);
    const auto embedded_null = value.find('\0');
    if (embedded_null != std::string::npos) {
        if (!std::all_of(value.begin() + static_cast<std::ptrdiff_t>(embedded_null),
                value.end(), [](char ch) { return ch == '\0'; })) {
            return {};
        }
        value.resize(embedded_null);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    std::size_t leading{};
    while (leading < value.size() &&
        std::isspace(static_cast<unsigned char>(value[leading]))) ++leading;
    if (leading != 0) value.erase(0, leading);
    if (value.size() == 24 && value.find('-') == std::string::npos &&
        value.find('&') == std::string::npos) {
        value.insert(8, "-");
    }
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool apple_usb_serial_equal(std::string_view left, std::string_view right) noexcept {
    try {
        const auto normalized_left = normalize_apple_usb_serial(left);
        const auto normalized_right = normalize_apple_usb_serial(right);
        return !normalized_left.empty() && !normalized_right.empty() &&
            normalized_left == normalized_right;
    } catch (...) {
        // This helper is also used by the C ABI readiness probe. Treat an
        // allocation failure as a non-match instead of allowing an exception
        // to cross a noexcept/native boundary.
        return false;
    }
}

} // namespace iPhoneMirror::transport
