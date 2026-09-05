// SPDX-License-Identifier: GPL-3.0-only

#include "Device/LinuxUdevMonitor.h"

#include <libudev.h>
#include <poll.h>

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace iPhoneMirror::device {
namespace {

constexpr std::uint16_t AppleVendorId = 0x05ac;

std::string attribute(udev_device* device, const char* name) {
    const char* value = udev_device_get_sysattr_value(device, name);
    return value != nullptr ? std::string(value) : std::string{};
}

std::optional<std::uint32_t> parse_hex(std::string_view text) noexcept {
    std::uint32_t value{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

std::optional<std::uint32_t> parse_decimal(std::string_view text) noexcept {
    std::uint32_t value{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

// The devpath of a USB device ends in the port chain, for example
// .../usb1/1-3/1-3.2. That chain identifies the physical port and survives the
// re-enumeration that changes the address and possibly the product id.
std::string port_path_of(udev_device* device) {
    const char* devpath = udev_device_get_devpath(device);
    if (devpath == nullptr) return {};
    std::string_view path(devpath);
    const auto last_separator = path.find_last_of('/');
    if (last_separator == std::string_view::npos) return std::string(path);
    return std::string(path.substr(last_separator + 1));
}

UdevAction action_of(udev_device* device) noexcept {
    const char* action = udev_device_get_action(device);
    if (action == nullptr) return UdevAction::Other;
    const std::string_view text(action);
    if (text == "add") return UdevAction::Add;
    if (text == "bind") return UdevAction::Bind;
    if (text == "remove") return UdevAction::Remove;
    if (text == "change") return UdevAction::Change;
    if (text == "unbind") return UdevAction::Unbind;
    return UdevAction::Other;
}

std::optional<UdevAppleDevice> describe(udev_device* device) {
    if (device == nullptr) return std::nullopt;
    const char* devtype = udev_device_get_devtype(device);
    if (devtype == nullptr || std::string_view(devtype) != "usb_device")
        return std::nullopt;

    const auto vendor = parse_hex(attribute(device, "idVendor"));
    if (!vendor || *vendor != AppleVendorId) return std::nullopt;

    UdevAppleDevice result;
    const char* syspath = udev_device_get_syspath(device);
    if (syspath != nullptr) result.syspath = syspath;
    result.port_path = port_path_of(device);
    result.serial = attribute(device, "serial");
    result.vendor_id = static_cast<std::uint16_t>(*vendor);
    if (const auto product = parse_hex(attribute(device, "idProduct")))
        result.product_id = static_cast<std::uint16_t>(*product);
    if (const auto bus = parse_decimal(attribute(device, "busnum")))
        result.bus = static_cast<std::uint8_t>(*bus);
    if (const auto address = parse_decimal(attribute(device, "devnum")))
        result.address = static_cast<std::uint8_t>(*address);
    // An unconfigured device reports 0 here, and the attribute can be missing
    // entirely while the kernel is still setting the device up. Both mean "not
    // configured", which the recovery policy distinguishes from a known value.
    const auto configuration = attribute(device, "bConfigurationValue");
    if (!configuration.empty()) {
        if (const auto value = parse_decimal(configuration))
            result.active_configuration = static_cast<std::uint8_t>(*value);
    }
    if (const auto count = parse_decimal(attribute(device, "bNumConfigurations")))
        result.configuration_count = static_cast<std::uint8_t>(*count);
    return result;
}

} // namespace

struct UdevAppleMonitor::Impl {
    udev* context{};
    udev_monitor* monitor{};
    int fd{-1};

    ~Impl() {
        if (monitor != nullptr) udev_monitor_unref(monitor);
        if (context != nullptr) udev_unref(context);
    }
};

UdevAppleMonitor::UdevAppleMonitor() : impl_(std::make_unique<Impl>()) {
    impl_->context = udev_new();
    if (impl_->context == nullptr) return;
    impl_->monitor = udev_monitor_new_from_netlink(impl_->context, "udev");
    if (impl_->monitor == nullptr) return;
    // usb_device only: interface-level events fire several times per device and
    // carry none of the attributes the recovery policy reads.
    if (udev_monitor_filter_add_match_subsystem_devtype(
            impl_->monitor, "usb", "usb_device") < 0) {
        return;
    }
    if (udev_monitor_enable_receiving(impl_->monitor) < 0) return;
    impl_->fd = udev_monitor_get_fd(impl_->monitor);
}

UdevAppleMonitor::~UdevAppleMonitor() = default;

bool UdevAppleMonitor::valid() const noexcept {
    return impl_ != nullptr && impl_->fd >= 0;
}

std::vector<UdevAppleDevice> UdevAppleMonitor::enumerate() const {
    std::vector<UdevAppleDevice> result;
    if (impl_ == nullptr || impl_->context == nullptr) return result;

    udev_enumerate* enumerator = udev_enumerate_new(impl_->context);
    if (enumerator == nullptr) return result;
    udev_enumerate_add_match_subsystem(enumerator, "usb");
    udev_enumerate_add_match_property(enumerator, "DEVTYPE", "usb_device");
    if (udev_enumerate_scan_devices(enumerator) >= 0) {
        udev_list_entry* entry{};
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerator)) {
            const char* syspath = udev_list_entry_get_name(entry);
            if (syspath == nullptr) continue;
            udev_device* device = udev_device_new_from_syspath(impl_->context,
                syspath);
            if (device == nullptr) continue;
            if (auto described = describe(device))
                result.push_back(std::move(*described));
            udev_device_unref(device);
        }
    }
    udev_enumerate_unref(enumerator);
    return result;
}

std::optional<UdevEvent> UdevAppleMonitor::wait_for_event(
    std::chrono::milliseconds timeout) {
    if (!valid()) return std::nullopt;

    pollfd descriptor{};
    descriptor.fd = impl_->fd;
    descriptor.events = POLLIN;
    const auto ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (ready <= 0) return std::nullopt;

    udev_device* device = udev_monitor_receive_device(impl_->monitor);
    if (device == nullptr) return std::nullopt;
    // A non-Apple device on the same bus is not an error; the caller polls again.
    auto described = describe(device);
    std::optional<UdevEvent> result;
    if (described) {
        result = UdevEvent{
            .action = action_of(device),
            .device = std::move(*described),
        };
    }
    udev_device_unref(device);
    return result;
}

std::optional<UdevAppleDevice> read_apple_device(const std::string& syspath) {
    udev* context = udev_new();
    if (context == nullptr) return std::nullopt;
    std::optional<UdevAppleDevice> result;
    udev_device* device = udev_device_new_from_syspath(context, syspath.c_str());
    if (device != nullptr) {
        result = describe(device);
        udev_device_unref(device);
    }
    udev_unref(context);
    return result;
}

} // namespace iPhoneMirror::device
