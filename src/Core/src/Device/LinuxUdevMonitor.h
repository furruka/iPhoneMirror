// SPDX-License-Identifier: GPL-3.0-only
//
// libudev-backed observation of Apple USB devices.
//
// Polling libusb would answer "is the device there" but not "did it just
// re-appear", and the difference matters: the 0x52 vendor request detaches the
// device, and the host must act in the window right after udev has processed the
// new add event. Watching the netlink socket gives that edge directly instead of
// inferring it from two polls that might both fall on the same side of it.
//
// The monitor also carries the sysfs values the recovery policy needs, because
// reading bConfigurationValue from sysfs does not require opening the device and
// therefore cannot fail on a node udev has not granted yet.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iPhoneMirror::device {

enum class UdevAction {
    Add,
    Bind,
    Remove,
    Change,
    Unbind,
    Other,
};

struct UdevAppleDevice {
    std::string syspath;
    // Stable across the 0x52 re-enumeration, unlike the USB address or the
    // product id: the same physical port keeps the same devpath prefix.
    std::string port_path;
    std::string serial;
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus{};
    std::uint8_t address{};
    // Read from sysfs bConfigurationValue. Unset when the attribute is absent,
    // which is how an unconfigured device presents.
    std::optional<std::uint8_t> active_configuration;
    std::uint8_t configuration_count{};
};

struct UdevEvent {
    UdevAction action{UdevAction::Other};
    UdevAppleDevice device;
};

// Read-only monitor. Construction opens the netlink socket and applies the
// filter; nothing is written to sysfs and no device is opened.
class UdevAppleMonitor final {
public:
    UdevAppleMonitor();
    ~UdevAppleMonitor();
    UdevAppleMonitor(const UdevAppleMonitor&) = delete;
    UdevAppleMonitor& operator=(const UdevAppleMonitor&) = delete;

    [[nodiscard]] bool valid() const noexcept;

    // Current Apple USB devices, from the same source as the events so the two
    // views cannot disagree about a field.
    [[nodiscard]] std::vector<UdevAppleDevice> enumerate() const;

    // Waits up to `timeout` for the next event. Returns nullopt on timeout,
    // which is the normal case while nothing is being plugged or switched.
    [[nodiscard]] std::optional<UdevEvent> wait_for_event(
        std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Re-reads one device's sysfs state without waiting for an event. Returns
// nullopt when the syspath has gone away, which is what an in-flight
// re-enumeration looks like.
[[nodiscard]] std::optional<UdevAppleDevice> read_apple_device(
    const std::string& syspath);

} // namespace iPhoneMirror::device
