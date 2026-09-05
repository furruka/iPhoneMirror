// SPDX-License-Identifier: GPL-3.0-only
//
// Linux DeviceManager. The Windows implementation reads SetupAPI PnP state and
// talks to Apple's usbmux service over loopback TCP; here the same record is
// filled from usbmuxd over AF_UNIX plus libusb enumeration.
//
// Field mapping, where the Linux answer is not simply the Windows one:
//  - apple_mobile_device_service_* becomes usbmuxd: installed means its socket
//    exists, running means it accepted a connection.
//  - capture_usbmux has no Linux counterpart. Windows exposes a second mux for
//    capture; Linux has exactly one usbmuxd, so this stays false.
//  - usbdk_backend is reported as known-and-unavailable: UsbDk is a Windows
//    kernel filter, and "we know there is none" is the honest answer rather
//    than leaving the flag unprobed.

#include "Device/DeviceManager.h"

#include "Device/LinuxEnvironmentProbe.h"
#include "Text/Utf.h"
#include "Transport/AppleUsbSerial.h"
#include "Transport/QtUsbTransport.h"
#include "Transport/UsbMuxClient.h"

#include <format>
#include <map>
#include <utility>

namespace iPhoneMirror::device {
namespace {

std::vector<transport::MuxDevice> list_usbmux_devices() noexcept {
    try {
        transport::UsbMuxClient mux{std::string(transport::UsbMuxUnixSocketPath)};
        return mux.list_devices();
    } catch (...) {
        return {};
    }
}

std::vector<transport::AppleUsbDevice> enumerate_apple_usb_devices() noexcept {
    try {
        transport::QtUsbContext context(false);
        return context.enumerate();
    } catch (...) {
        return {};
    }
}

} // namespace

EnvironmentRecord DeviceManager::environment() const {
    EnvironmentRecord result;

    const auto probe = linux_probe::probe();
    result.service_installed =
        probe.usbmuxd != linux_probe::UsbmuxdAvailability::NotInstalled;
    result.service_running =
        probe.usbmuxd == linux_probe::UsbmuxdAvailability::Connected;
    result.standard_mux = result.service_running;
    result.capture_mux = false;
    result.physical_device_count = probe.usb_access.apple_nodes_seen;

    const auto usb_runtime = transport::probe_usb_runtime();
    result.libusb_runtime = usb_runtime.runtime_available;
    result.libusb_version = text::utf8_to_wide(usb_runtime.version);
    // The automatic environment poll deliberately does not enumerate; the probe
    // above already counted Apple nodes while checking access.
    result.libusb_apple_devices_known = true;
    result.libusb_apple_devices = probe.usb_access.apple_nodes_seen;
    result.usbdk_backend_known = true;
    result.usbdk_backend = false;
    result.libusb0_available = false;
    result.libusb0_apple_devices_known = true;
    result.libusb0_apple_devices = 0;

    result.diagnostic = linux_probe::describe(probe);
    if (result.libusb_runtime) {
        result.diagnostic += L" libusb " + result.libusb_version + L" 已加载。";
    } else {
        result.diagnostic += L" libusb 用户态运行库不可用。";
    }
    if (!linux_probe::wired_capture_ready(probe)) {
        result.diagnostic += L" 有线采集尚不可用。";
    }
    return result;
}

std::vector<DeviceRecord> DeviceManager::refresh(bool refresh_metadata) {
    // Lockdown metadata (device name, product type, iOS version) needs a paired
    // lockdownd session, which this port does not open yet; usbmuxd still
    // supplies identity and connection type without pairing.
    (void)refresh_metadata;

    std::map<std::string, DeviceRecord, std::less<>> records;
    for (const auto& mux_device : list_usbmux_devices()) {
        const auto serial = transport::normalize_apple_usb_serial(mux_device.serial);
        if (serial.empty()) continue;
        DeviceRecord record;
        record.device_id = mux_device.device_id;
        record.mux_port = 0; // AF_UNIX endpoint; no TCP port applies.
        record.usb_connected = mux_device.connection_type == "USB";
        record.pair_record_present = false;
        record.lockdown_accessible = false;
        record.state = record.usb_connected ? ConnectionState::Connected
                                            : ConnectionState::Disconnected;
        record.udid = text::utf8_to_wide(mux_device.serial);
        record.connection_type = text::utf8_to_wide(mux_device.connection_type);
        record.status = L"usbmuxd 已识别设备；配对与 Lockdown 元数据尚未实现。";
        records.emplace(serial, std::move(record));
    }

    // A device can be visible over USB while usbmuxd has not claimed it, which
    // is exactly the state the hidden capture configuration leaves it in.
    for (const auto& usb_device : enumerate_apple_usb_devices()) {
        const auto serial = transport::normalize_apple_usb_serial(usb_device.serial);
        if (serial.empty()) continue;
        const auto found = records.find(serial);
        if (found != records.end()) {
            found->second.usb_connected = true;
            continue;
        }
        DeviceRecord record;
        record.mux_port = 0;
        record.usb_connected = true;
        record.state = ConnectionState::UsbPresentNoMux;
        record.udid = text::utf8_to_wide(usb_device.serial);
        record.connection_type = L"USB";
        record.status = usb_device.quicktime_configuration
            ? L"设备暴露 QuickTime 采集配置，但 usbmuxd 未识别。"
            : L"设备已连接，但 usbmuxd 未识别；请检查 udev 权限与 usbmuxd 状态。";
        records.emplace(serial, std::move(record));
    }

    std::vector<DeviceRecord> result;
    result.reserve(records.size());
    for (auto& [serial, record] : records) result.push_back(std::move(record));
    return result;
}

} // namespace iPhoneMirror::device
