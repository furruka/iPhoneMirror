// SPDX-License-Identifier: GPL-3.0-only

#include "Device/LinuxEnvironmentProbe.h"

#include "Text/Utf.h"
#include "Transport/Socket.h"
#include "Transport/UsbMuxClient.h"

#include <libusb.h>

#include <grp.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <vector>

namespace iPhoneMirror::device::linux_probe {
namespace {

constexpr std::uint16_t AppleVendorId = 0x05ac;

// This project's rule. It must grant the device node to a group the desktop
// user is in, because usbmuxd's 39-usbmuxd.rules sets OWNER="usbmux".
constexpr std::string_view ProjectUdevRuleName = "70-iphonemirror.rules";
constexpr std::array<std::string_view, 2> UdevRuleDirectories{
    "/etc/udev/rules.d",
    "/usr/lib/udev/rules.d",
};

// Evidence that the usbmuxd package is present even when the daemon is not
// running. Checked in this order so the reported path is the most useful one.
constexpr std::array<std::string_view, 5> UsbmuxdEvidencePaths{
    "/usr/lib/systemd/system/usbmuxd.service",
    "/lib/systemd/system/usbmuxd.service",
    "/usr/sbin/usbmuxd",
    "/usr/bin/usbmuxd",
    "/usr/lib/udev/rules.d/39-usbmuxd.rules",
};

std::vector<std::string> current_user_groups() noexcept {
    std::vector<std::string> names;
    try {
        int count = ::getgroups(0, nullptr);
        if (count < 0) return names;
        std::vector<gid_t> gids(static_cast<std::size_t>(count));
        if (count > 0) {
            count = ::getgroups(count, gids.data());
            if (count < 0) return names;
            gids.resize(static_cast<std::size_t>(count));
        }
        // getgroups() may omit the real gid on some systems; add it explicitly.
        gids.push_back(::getgid());
        names.reserve(gids.size());
        for (const auto gid : gids) {
            const struct group* entry = ::getgrgid(gid);
            if (entry != nullptr && entry->gr_name != nullptr)
                names.emplace_back(entry->gr_name);
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    } catch (...) {
    }
    return names;
}

bool contains(const std::vector<std::string>& values, std::string_view name) noexcept {
    return std::find(values.begin(), values.end(), name) != values.end();
}

// Counts Apple devices and how many can actually be opened. libusb reports
// LIBUSB_ERROR_ACCESS for a node udev has not granted, which is precisely the
// distinction this probe exists to surface.
UsbAccessFinding probe_usb_access(bool running_as_root) noexcept {
    UsbAccessFinding finding;
    libusb_context* context{};
    if (libusb_init(&context) != LIBUSB_SUCCESS) return finding;

    libusb_device** devices{};
    const auto count = libusb_get_device_list(context, &devices);
    for (ssize_t index = 0; index < count; ++index) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) != LIBUSB_SUCCESS)
            continue;
        if (descriptor.idVendor != AppleVendorId) continue;
        ++finding.apple_nodes_seen;
        libusb_device_handle* handle{};
        if (libusb_open(devices[index], &handle) == LIBUSB_SUCCESS) {
            ++finding.apple_nodes_openable;
            libusb_close(handle);
        }
    }
    if (devices != nullptr) libusb_free_device_list(devices, 1);
    libusb_exit(context);

    if (finding.apple_nodes_seen == 0) {
        // Nothing to judge yet. The rule check still reports whether the
        // environment is prepared for the first device.
        finding.access = UsbAccess::Unknown;
    } else if (finding.apple_nodes_openable == finding.apple_nodes_seen) {
        finding.access = running_as_root ? UsbAccess::GrantedAsRoot
                                         : UsbAccess::Granted;
    } else {
        finding.access = UsbAccess::DeniedNeedsUdevRule;
    }
    return finding;
}

UsbmuxdAvailability probe_usbmuxd(const std::string& path,
    std::string& evidence_path) noexcept {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
        try {
            auto socket = transport::Socket::connect_unix(path, 250);
            if (socket.valid()) return UsbmuxdAvailability::Connected;
        } catch (...) {
        }
        return UsbmuxdAvailability::SocketPresentNotAccepting;
    }
    for (const auto candidate : UsbmuxdEvidencePaths) {
        error.clear();
        if (std::filesystem::exists(candidate, error) && !error) {
            evidence_path = std::string(candidate);
            return UsbmuxdAvailability::InstalledNotRunning;
        }
    }
    return UsbmuxdAvailability::NotInstalled;
}

} // namespace

Report probe() noexcept {
    Report report;
    report.usbmuxd_socket_path = std::string(transport::UsbMuxUnixSocketPath);
    report.usbmuxd = probe_usbmuxd(report.usbmuxd_socket_path,
        report.usbmuxd_evidence_path);

    for (const auto directory : UdevRuleDirectories) {
        auto candidate = std::filesystem::path(directory) /
            std::string(ProjectUdevRuleName);
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) {
            report.project_udev_rule_installed = true;
            report.project_udev_rule_path = candidate.string();
            break;
        }
    }

    report.user_groups = current_user_groups();
    report.member_of_usbmux_group = contains(report.user_groups, "usbmux");
    report.member_of_plugdev_group = contains(report.user_groups, "plugdev");
    report.usb_access = probe_usb_access(::geteuid() == 0);
    return report;
}

bool wired_capture_ready(const Report& report) noexcept {
    // usbmuxd supplies the device serial and product id without pairing, so it
    // has to be installed. It does not have to be running yet: udev starts it
    // when an Apple device is attached.
    if (report.usbmuxd == UsbmuxdAvailability::NotInstalled) return false;
    if (report.usbmuxd == UsbmuxdAvailability::SocketPresentNotAccepting)
        return false;

    switch (report.usb_access.access) {
    case UsbAccess::Granted:
    case UsbAccess::GrantedAsRoot:
        // A device is attached and openable, so udev has already granted it.
        // usbmuxd must be running by now for discovery to work.
        return report.usbmuxd == UsbmuxdAvailability::Connected;
    case UsbAccess::Unknown:
        // No device attached: ready if the environment is prepared for one.
        return report.project_udev_rule_installed;
    case UsbAccess::DeniedNeedsUdevRule:
        return false;
    }
    return false;
}

std::wstring describe(const Report& report) {
    std::string text;

    switch (report.usbmuxd) {
    case UsbmuxdAvailability::Connected:
        text += std::format("usbmuxd 已就绪（{}）。", report.usbmuxd_socket_path);
        break;
    case UsbmuxdAvailability::InstalledNotRunning:
        text += std::format(
            "usbmuxd 已安装（{}）但尚未运行；它由 udev 在插入 Apple 设备时启动，"
            "当前无设备时这是正常状态。",
            report.usbmuxd_evidence_path);
        break;
    case UsbmuxdAvailability::SocketPresentNotAccepting:
        text += std::format(
            "usbmuxd 套接字存在但拒绝连接（{}）；请检查 usbmuxd.service 状态。",
            report.usbmuxd_socket_path);
        break;
    case UsbmuxdAvailability::NotInstalled:
        text += std::format(
            "未检测到 usbmuxd（既无套接字 {} 也无守护进程/服务文件）；请安装 usbmuxd。",
            report.usbmuxd_socket_path);
        break;
    }

    switch (report.valeria_mux) {
    case ValeriaMuxSupport::Verified:
        text += " usbmuxd 与采集配置共存已验证（它支持 Valeria）。";
        break;
    case ValeriaMuxSupport::Rejected:
        text += " 检测到设备处于采集配置，但 usbmuxd 没有连接它——这台机器上的 usbmuxd "
                "不支持 Valeria（1.1.1 会把配置号钳到 4 并放弃设备），iOS 因此拿不到"
                "它需要的 lockdown 会话。请换用支持 Valeria 的 usbmuxd（git master 之后）。";
        break;
    case ValeriaMuxSupport::Unknown:
        text += " 有线采集要求 usbmuxd 支持 Valeria 配置（1.1.1 不行）；当前尚未观察到"
                "可判定的证据。";
        break;
    }


    switch (report.usb_access.access) {
    case UsbAccess::Granted:
        text += std::format(" Apple USB 设备节点可打开（{}/{}）。",
            report.usb_access.apple_nodes_openable,
            report.usb_access.apple_nodes_seen);
        break;
    case UsbAccess::GrantedAsRoot:
        text += std::format(
            " 以 root 运行，Apple USB 设备节点可打开（{}/{}）；这绕过了 udev，"
            "普通用户运行前仍需安装 udev 规则。",
            report.usb_access.apple_nodes_openable,
            report.usb_access.apple_nodes_seen);
        break;
    case UsbAccess::DeniedNeedsUdevRule:
        text += std::format(
            " 检测到 {} 个 Apple USB 设备，但只有 {} 个可打开；usbmuxd 的 udev 规则把"
            "设备节点归给 usbmux 用户，需要安装本项目的 udev 规则并把当前用户加入相应组。",
            report.usb_access.apple_nodes_seen,
            report.usb_access.apple_nodes_openable);
        break;
    case UsbAccess::Unknown:
        text += " 当前未连接 Apple USB 设备，无法验证设备节点权限。";
        break;
    }

    if (report.project_udev_rule_installed) {
        text += std::format(" 已安装 udev 规则：{}。",
            report.project_udev_rule_path);
    } else {
        text += std::format(
            " 未安装 udev 规则 {}（见 docs/LINUX_PORT.md）。", ProjectUdevRuleName);
    }

    if (report.member_of_usbmux_group) text += " 当前用户属于 usbmux 组。";
    if (report.member_of_plugdev_group) text += " 当前用户属于 plugdev 组。";

    return text::utf8_to_wide(text);
}

} // namespace iPhoneMirror::device::linux_probe
