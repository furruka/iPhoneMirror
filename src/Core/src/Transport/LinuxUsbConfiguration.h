// SPDX-License-Identifier: GPL-3.0-only
//
// Setting the active USB configuration on Linux and claiming the QuickTime
// interface in the same breath.
//
// Both halves live here because the measurement says they cannot be separated.
// On an iPhone 16 Pro, with usbmuxd running: SET_CONFIGURATION for the Valeria
// configuration succeeded at 14:51:18.391, and the claim 14 ms later failed with
// LIBUSB_ERROR_BUSY because usbmuxd had selected its own configuration in
// between ("Connected to v2.0 device 1" at 14:51:18.406). Those 14 ms were spent
// in the shared open path, which enumerates every Apple device and opens each one
// to read its serial. So the claim has to happen on the handle that set the
// configuration, with nothing in between.
//
// Windows needs none of this: AppleUsbFilter leaves the selected configuration
// alone and there is no second daemon competing for it.

#pragma once

#include "Transport/QtUsbTransport.h"

#include <cstdint>
#include <span>
#include <string>

namespace iPhoneMirror::transport {

struct SetConfigurationResult {
    bool applied{};
    // libusb error name when the request failed, or a short explanation when the
    // device could not be located. Empty on success.
    std::string diagnostic;
};

// Re-issues SET_CONFIGURATION for the Apple device at the given bus/address.
// Used for diagnosis and for the case where the interface is claimed elsewhere;
// prefer ClaimedQuickTimeInterface, which does not leave a gap.
[[nodiscard]] SetConfigurationResult set_active_configuration(
    QtUsbContext& context, std::uint8_t bus, std::uint8_t address,
    std::uint8_t configuration) noexcept;

// Owns a libusb handle whose configuration was set and whose QuickTime interface
// is claimed. Deliberately not built on QtUsbConnection: that type's open path is
// what loses the race.
class ClaimedQuickTimeInterface final {
public:
    ClaimedQuickTimeInterface() = default;
    ~ClaimedQuickTimeInterface();
    ClaimedQuickTimeInterface(const ClaimedQuickTimeInterface&) = delete;
    ClaimedQuickTimeInterface& operator=(const ClaimedQuickTimeInterface&) = delete;
    ClaimedQuickTimeInterface(ClaimedQuickTimeInterface&& other) noexcept;
    ClaimedQuickTimeInterface& operator=(ClaimedQuickTimeInterface&& other) noexcept;

    // Opens the device at bus/address, selects `configuration`, and claims the
    // interface whose class/subclass is 0xFF/0x2A in that configuration. The
    // endpoints come from the active configuration descriptor, so the caller
    // does not have to have read them beforehand.
    [[nodiscard]] static ClaimedQuickTimeInterface open(QtUsbContext& context,
        std::uint8_t bus, std::uint8_t address, std::uint8_t configuration,
        std::string& diagnostic) noexcept;

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] const UsbEndpointSet& endpoints() const noexcept {
        return endpoints_;
    }
    // True when the configuration already matched and no SET_CONFIGURATION was
    // needed, which the caller reports because it changes what the run proves.
    [[nodiscard]] bool configuration_was_set() const noexcept {
        return configuration_was_set_;
    }

    [[nodiscard]] std::size_t read(std::span<std::uint8_t> destination,
        unsigned timeout_ms, std::string& diagnostic) noexcept;
    [[nodiscard]] bool write(std::span<const std::uint8_t> source,
        unsigned timeout_ms, std::string& diagnostic) noexcept;
    // Vendor control request the reference clients send when a freshly activated
    // endpoint has not started talking.
    [[nodiscard]] bool kick_handshake(std::string& diagnostic) noexcept;
    // 0x52 with wIndex=0: asks iOS to drop the Valeria configuration again.
    [[nodiscard]] bool request_normal_configuration() noexcept;
    void close() noexcept;

private:
    libusb_device_handle* handle_{};
    UsbEndpointSet endpoints_{};
    std::uint8_t claimed_interface_{};
    bool claimed_{};
    bool configuration_was_set_{};
};

} // namespace iPhoneMirror::transport
