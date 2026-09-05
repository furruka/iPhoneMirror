// SPDX-License-Identifier: GPL-3.0-only

#include "Transport/LinuxUsbConfiguration.h"

#include <libusb.h>

#include <format>
#include <memory>

namespace iPhoneMirror::transport {

SetConfigurationResult set_active_configuration(QtUsbContext& context,
    std::uint8_t bus, std::uint8_t address, std::uint8_t configuration) noexcept {
    SetConfigurationResult result;
    try {
        libusb_device** devices{};
        const auto count = libusb_get_device_list(context.native(), &devices);
        if (count < 0) {
            result.diagnostic = std::format("libusb_get_device_list: {}",
                libusb_error_name(static_cast<int>(count)));
            return result;
        }
        std::unique_ptr<libusb_device*, void (*)(libusb_device**)> list_guard(
            devices, [](libusb_device** list) {
                if (list != nullptr) libusb_free_device_list(list, 1);
            });

        libusb_device* target{};
        for (ssize_t index = 0; index < count; ++index) {
            if (libusb_get_bus_number(devices[index]) == bus &&
                libusb_get_device_address(devices[index]) == address) {
                target = devices[index];
                break;
            }
        }
        if (target == nullptr) {
            // The device is mid-re-enumeration. The caller re-samples; this is
            // not an error condition on its own.
            result.diagnostic = "device is not enumerated at this bus/address";
            return result;
        }

        libusb_device_handle* handle{};
        const int open_result = libusb_open(target, &handle);
        if (open_result != LIBUSB_SUCCESS) {
            result.diagnostic = std::format("libusb_open: {}",
                libusb_error_name(open_result));
            return result;
        }
        std::unique_ptr<libusb_device_handle, void (*)(libusb_device_handle*)>
            handle_guard(handle, [](libusb_device_handle* value) {
                if (value != nullptr) libusb_close(value);
            });

        const int set_result = libusb_set_configuration(handle, configuration);
        if (set_result == LIBUSB_SUCCESS) {
            result.applied = true;
            return result;
        }
        // BUSY means a kernel driver holds an interface. usbmuxd claims the mux
        // interface, so this is the expected contention rather than a fault, and
        // the caller retries after the next event.
        result.diagnostic = std::format("libusb_set_configuration: {}",
            libusb_error_name(set_result));
        return result;
    } catch (...) {
        result.diagnostic = "unexpected exception while setting the configuration";
        return result;
    }
}

} // namespace iPhoneMirror::transport
