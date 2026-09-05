#pragma once

// apple_usb_serial_equal moved to Transport/AppleUsbSerial.h so the shared
// capture session can use it without pulling in the libusb0 backend.
#include "Transport/AppleUsbSerial.h"

#include <string_view>

namespace iPhoneMirror::transport {

// Read-only exact-device probe. This only enumerates libusb0 devices and opens
// their descriptors to read serial strings; it never changes USB state.
[[nodiscard]] bool is_libusb0_device_available(std::string_view serial);

} // namespace iPhoneMirror::transport
