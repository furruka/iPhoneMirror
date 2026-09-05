// SPDX-License-Identifier: GPL-3.0-only

#include "Transport/LinuxUsbConfiguration.h"

#include <libusb.h>

#include <algorithm>
#include <format>
#include <memory>
#include <utility>

namespace iPhoneMirror::transport {
namespace {

constexpr std::uint8_t VendorInterfaceClass = 0xff;
constexpr std::uint8_t QuickTimeSubclass = 0x2a;

using DeviceListGuard =
    std::unique_ptr<libusb_device*, void (*)(libusb_device**)>;

DeviceListGuard make_list_guard(libusb_device** devices) {
    return DeviceListGuard(devices, [](libusb_device** list) {
        if (list != nullptr) libusb_free_device_list(list, 1);
    });
}

libusb_device* find_at(libusb_device** devices, ssize_t count, std::uint8_t bus,
    std::uint8_t address) noexcept {
    for (ssize_t index = 0; index < count; ++index) {
        if (libusb_get_bus_number(devices[index]) == bus &&
            libusb_get_device_address(devices[index]) == address) {
            return devices[index];
        }
    }
    return nullptr;
}

// Locates the 0xFF/0x2A interface in the currently active configuration and
// reports its bulk endpoint pair.
bool describe_quicktime_interface(libusb_device* device, UsbEndpointSet& endpoints,
    std::uint8_t& interface_number) noexcept {
    libusb_config_descriptor* raw{};
    if (libusb_get_active_config_descriptor(device, &raw) != LIBUSB_SUCCESS)
        return false;
    std::unique_ptr<libusb_config_descriptor,
        void (*)(libusb_config_descriptor*)> config(raw,
            [](libusb_config_descriptor* value) {
                if (value != nullptr) libusb_free_config_descriptor(value);
            });

    for (std::uint8_t group_index = 0; group_index < config->bNumInterfaces;
        ++group_index) {
        const auto& group = config->interface[group_index];
        for (int alternate = 0; alternate < group.num_altsetting; ++alternate) {
            const auto& descriptor = group.altsetting[alternate];
            if (descriptor.bInterfaceClass != VendorInterfaceClass ||
                descriptor.bInterfaceSubClass != QuickTimeSubclass) continue;
            UsbEndpointSet candidate{};
            candidate.configuration = config->bConfigurationValue;
            candidate.interface_number = descriptor.bInterfaceNumber;
            candidate.alternate_setting = descriptor.bAlternateSetting;
            for (std::uint8_t index = 0; index < descriptor.bNumEndpoints; ++index) {
                const auto& endpoint = descriptor.endpoint[index];
                if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_BULK) continue;
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0) {
                    candidate.bulk_in = endpoint.bEndpointAddress;
                    candidate.bulk_in_packet_size = endpoint.wMaxPacketSize;
                } else {
                    candidate.bulk_out = endpoint.bEndpointAddress;
                    candidate.bulk_out_packet_size = endpoint.wMaxPacketSize;
                }
            }
            if (candidate.bulk_in == 0 || candidate.bulk_out == 0) continue;
            endpoints = candidate;
            interface_number = descriptor.bInterfaceNumber;
            return true;
        }
    }
    return false;
}

} // namespace

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
        auto list_guard = make_list_guard(devices);

        libusb_device* target = find_at(devices, count, bus, address);
        if (target == nullptr) {
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
        // BUSY means a kernel driver or another process holds an interface.
        // usbmuxd claims the mux interface, so this is expected contention.
        result.diagnostic = std::format("libusb_set_configuration: {}",
            libusb_error_name(set_result));
        return result;
    } catch (...) {
        result.diagnostic = "unexpected exception while setting the configuration";
        return result;
    }
}

ClaimedQuickTimeInterface::~ClaimedQuickTimeInterface() { close(); }

ClaimedQuickTimeInterface::ClaimedQuickTimeInterface(
    ClaimedQuickTimeInterface&& other) noexcept
    : handle_(other.handle_), endpoints_(other.endpoints_),
      claimed_interface_(other.claimed_interface_), claimed_(other.claimed_),
      configuration_was_set_(other.configuration_was_set_) {
    other.handle_ = nullptr;
    other.claimed_ = false;
}

ClaimedQuickTimeInterface& ClaimedQuickTimeInterface::operator=(
    ClaimedQuickTimeInterface&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        endpoints_ = other.endpoints_;
        claimed_interface_ = other.claimed_interface_;
        claimed_ = other.claimed_;
        configuration_was_set_ = other.configuration_was_set_;
        other.handle_ = nullptr;
        other.claimed_ = false;
    }
    return *this;
}

ClaimedQuickTimeInterface ClaimedQuickTimeInterface::open(QtUsbContext& context,
    std::uint8_t bus, std::uint8_t address, std::uint8_t configuration,
    std::string& diagnostic) noexcept {
    ClaimedQuickTimeInterface result;
    try {
        libusb_device** devices{};
        const auto count = libusb_get_device_list(context.native(), &devices);
        if (count < 0) {
            diagnostic = std::format("libusb_get_device_list: {}",
                libusb_error_name(static_cast<int>(count)));
            return result;
        }
        auto list_guard = make_list_guard(devices);

        libusb_device* target = find_at(devices, count, bus, address);
        if (target == nullptr) {
            diagnostic = "device is not enumerated at this bus/address";
            return result;
        }

        libusb_device_handle* handle{};
        const int open_result = libusb_open(target, &handle);
        if (open_result != LIBUSB_SUCCESS) {
            diagnostic = std::format("libusb_open: {}",
                libusb_error_name(open_result));
            return result;
        }
        result.handle_ = handle;

        int active{};
        const bool active_known =
            libusb_get_configuration(handle, &active) == LIBUSB_SUCCESS;
        if (!active_known || active != configuration) {
            const int set_result = libusb_set_configuration(handle, configuration);
            if (set_result != LIBUSB_SUCCESS) {
                diagnostic = std::format("libusb_set_configuration: {}",
                    libusb_error_name(set_result));
                result.close();
                return result;
            }
            result.configuration_was_set_ = true;
        }

        // Read the endpoints from the configuration now active, so the caller
        // never has to re-enumerate: that re-enumeration is what cost the race.
        if (!describe_quicktime_interface(target, result.endpoints_,
                result.claimed_interface_)) {
            diagnostic = "the active configuration exposes no 0xFF/0x2A interface";
            result.close();
            return result;
        }

        const int claim_result =
            libusb_claim_interface(handle, result.claimed_interface_);
        if (claim_result != LIBUSB_SUCCESS) {
            diagnostic = std::format("libusb_claim_interface({}): {}",
                result.claimed_interface_, libusb_error_name(claim_result));
            result.close();
            return result;
        }
        result.claimed_ = true;

        if (result.endpoints_.alternate_setting != 0) {
            const int alternate_result = libusb_set_interface_alt_setting(handle,
                result.claimed_interface_, result.endpoints_.alternate_setting);
            if (alternate_result != LIBUSB_SUCCESS) {
                diagnostic = std::format("libusb_set_interface_alt_setting: {}",
                    libusb_error_name(alternate_result));
                result.close();
                return result;
            }
        }
        diagnostic.clear();
        return result;
    } catch (...) {
        diagnostic = "unexpected exception while claiming the interface";
        result.close();
        return result;
    }
}

std::size_t ClaimedQuickTimeInterface::read(std::span<std::uint8_t> destination,
    unsigned timeout_ms, std::string& diagnostic) noexcept {
    if (handle_ == nullptr || destination.empty()) return 0;
    int transferred{};
    const int result = libusb_bulk_transfer(handle_, endpoints_.bulk_in,
        destination.data(), static_cast<int>(destination.size()), &transferred,
        timeout_ms);
    if (result == LIBUSB_SUCCESS || result == LIBUSB_ERROR_TIMEOUT) {
        diagnostic.clear();
        return static_cast<std::size_t>(std::max(transferred, 0));
    }
    diagnostic = std::format("libusb_bulk_transfer(in): {}",
        libusb_error_name(result));
    return 0;
}

bool ClaimedQuickTimeInterface::write(std::span<const std::uint8_t> source,
    unsigned timeout_ms, std::string& diagnostic) noexcept {
    if (handle_ == nullptr) return false;
    std::size_t offset{};
    while (offset < source.size()) {
        int transferred{};
        const int result = libusb_bulk_transfer(handle_, endpoints_.bulk_out,
            const_cast<std::uint8_t*>(source.data()) + offset,
            static_cast<int>(source.size() - offset), &transferred, timeout_ms);
        if (result != LIBUSB_SUCCESS) {
            diagnostic = std::format("libusb_bulk_transfer(out): {} after {} of {}",
                libusb_error_name(result), offset, source.size());
            return false;
        }
        if (transferred <= 0) {
            diagnostic = "libusb_bulk_transfer(out) made no progress";
            return false;
        }
        offset += static_cast<std::size_t>(transferred);
    }
    diagnostic.clear();
    return true;
}

bool ClaimedQuickTimeInterface::kick_handshake(std::string& diagnostic) noexcept {
    if (handle_ == nullptr) return false;
    const int result = libusb_control_transfer(handle_, 0x40, 0x40, 0x6400, 0x6400,
        nullptr, 0, 1000);
    if (result >= 0) {
        diagnostic.clear();
        return true;
    }
    diagnostic = std::format("control(0x40/0x40): {}", libusb_error_name(result));
    return false;
}

bool ClaimedQuickTimeInterface::request_normal_configuration() noexcept {
    if (handle_ == nullptr) return false;
    const int result = libusb_control_transfer(handle_,
        static_cast<std::uint8_t>(LIBUSB_ENDPOINT_OUT) |
            static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR) |
            static_cast<std::uint8_t>(LIBUSB_RECIPIENT_DEVICE),
        0x52, 0, 0, nullptr, 0, 1000);
    // The request disconnects the device, so an I/O error here is not evidence
    // that it was rejected; re-enumeration is the authority.
    return result >= 0;
}

void ClaimedQuickTimeInterface::close() noexcept {
    if (handle_ == nullptr) return;
    if (claimed_) {
        (void)libusb_release_interface(handle_, claimed_interface_);
        claimed_ = false;
    }
    libusb_close(handle_);
    handle_ = nullptr;
}

} // namespace iPhoneMirror::transport

