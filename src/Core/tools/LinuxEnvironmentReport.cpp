// SPDX-License-Identifier: GPL-3.0-only
//
// Prints the Linux environment report through the real C ABI. This is the WP3
// acceptance tool: with no device attached it must explain accurately what is
// present and what is missing, and it must never claim capture is ready when it
// is not. It only reads state.

#include "iPhoneMirror/CoreApi.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string narrow(const wchar_t* text) {
    if (text == nullptr) return {};
    std::string result;
    for (const wchar_t* cursor = text; *cursor != L'\0'; ++cursor) {
        const auto code_point = static_cast<char32_t>(*cursor);
        if (code_point < 0x80U) {
            result.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800U) {
            result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point < 0x10000U) {
            result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
    }
    return result;
}

const char* yes_no(std::int32_t value) { return value != 0 ? "yes" : "no"; }

} // namespace

int main() {
    if (im_initialize() != 0) {
        std::printf("im_initialize failed: %s\n", narrow(im_last_error()).c_str());
        return 1;
    }

    std::printf("api_version           : %u\n", im_api_version());

    iPhoneMirror::EnvironmentInfo environment{};
    environment.struct_size = sizeof(environment);
    const auto environment_result = im_get_environment(&environment);
    if (environment_result != 0) {
        std::printf("im_get_environment failed (%d): %s\n", environment_result,
            narrow(im_last_error()).c_str());
        im_shutdown();
        return 1;
    }

    std::printf("usbmuxd installed     : %s\n",
        yes_no(environment.apple_mobile_device_service_installed));
    std::printf("usbmuxd running       : %s\n",
        yes_no(environment.apple_mobile_device_service_running));
    std::printf("standard mux          : %s\n",
        yes_no(environment.standard_usbmux_available));
    std::printf("capture mux           : %s (no Linux counterpart)\n",
        yes_no(environment.capture_usbmux_available));
    std::printf("apple usb devices     : %u\n",
        environment.physical_apple_usb_devices);
    std::printf("libusb runtime        : %s (%s)\n",
        yes_no(environment.libusb_runtime_available),
        narrow(environment.libusb_version).c_str());
    std::printf("libusb apple devices  : %u (known=%s)\n",
        environment.libusb_apple_devices,
        yes_no(environment.libusb_apple_devices_known));
    std::printf("usbdk backend         : %s (known=%s)\n",
        yes_no(environment.usbdk_backend_available),
        yes_no(environment.usbdk_backend_known));
    std::printf("diagnostic            : %s\n",
        narrow(environment.diagnostic).c_str());

    std::uint32_t count{};
    auto devices_result = im_refresh_devices(nullptr, &count);
    if (devices_result != 0) {
        std::printf("im_refresh_devices count query failed (%d): %s\n",
            devices_result, narrow(im_last_error()).c_str());
        im_shutdown();
        return 1;
    }
    std::printf("devices               : %u\n", count);
    if (count != 0) {
        std::vector<iPhoneMirror::DeviceInfo> devices(count);
        auto capacity = count;
        devices_result = im_refresh_devices(devices.data(), &capacity);
        if (devices_result != 0) {
            std::printf("im_refresh_devices failed (%d): %s\n", devices_result,
                narrow(im_last_error()).c_str());
            im_shutdown();
            return 1;
        }
        for (std::uint32_t index = 0; index < capacity; ++index) {
            const auto& device = devices[index];
            std::printf("  [%u] udid=%s state=%d usb=%s connection=%s\n", index,
                narrow(device.udid).c_str(), static_cast<int>(device.state),
                yes_no(device.usb_connected),
                narrow(device.connection_type).c_str());
            std::printf("       status=%s\n", narrow(device.status).c_str());
        }
    }

    // Capture must refuse rather than pretend. Report the refusal so the tool
    // proves the honest-failure contract instead of just describing it.
    const auto capture_result = im_start_capture(L"0000000000000000000000000");
    std::printf("im_start_capture      : %d (%s)\n", capture_result,
        narrow(im_last_error()).c_str());
    if (capture_result == 0) {
        std::printf("FAILED: capture reported success without an implementation\n");
        im_shutdown();
        return 1;
    }

    im_shutdown();
    return 0;
}
