// SPDX-License-Identifier: GPL-3.0-only
//
// Linux environment readiness probe. The Windows environment report answers
// "is Apple Mobile Device Support installed and running, and can a USB backend
// see the phone". The Linux answers come from different places, and two of them
// have no Windows counterpart at all:
//
//  - usbmuxd is a system service reachable over a unix-domain socket rather
//    than loopback TCP, and it may be socket-activated, so a missing socket and
//    a stopped service are different findings.
//  - Access to /dev/bus/usb is governed by udev. usbmuxd's own rule sets
//    OWNER="usbmux" on Apple devices, so an ordinary desktop user cannot open
//    the device node until this project's rule grants the group. That is the
//    single most common reason wired capture cannot start on Linux, so the
//    probe reports it explicitly instead of surfacing a generic open failure.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iPhoneMirror::device::linux_probe {

enum class UsbmuxdAvailability {
    // Neither the daemon nor its systemd unit is present.
    NotInstalled,
    // Installed but no socket yet. This is the normal idle state, not a fault:
    // usbmuxd is started by udev when an Apple device is attached
    // (39-usbmuxd.rules sets SYSTEMD_WANTS=usbmuxd.service), so with nothing
    // plugged in there is no socket to connect to.
    InstalledNotRunning,
    // The socket exists but nothing accepted the connection, which does point
    // at a failed or wedged daemon.
    SocketPresentNotAccepting,
    Connected,
};

// Which access path can open an Apple USB device node. The probe reports the
// reason rather than a bare yes/no because the fixes are different.
enum class UsbAccess {
    Unknown,
    // A udev rule granted this user access, directly or through a group.
    Granted,
    // The node exists but is owned by the usbmux user/group and this user is
    // not a member: this project's udev rule is missing or not yet reloaded.
    DeniedNeedsUdevRule,
    // Running as root bypasses udev entirely. Reported separately so the
    // diagnostic does not claim the rule is installed when it is not.
    GrantedAsRoot,
};

struct UsbAccessFinding {
    UsbAccess access{UsbAccess::Unknown};
    // Number of Apple USB device nodes examined under /dev/bus/usb.
    std::uint32_t apple_nodes_seen{};
    std::uint32_t apple_nodes_openable{};
};

struct Report {
    UsbmuxdAvailability usbmuxd{UsbmuxdAvailability::NotInstalled};
    std::string usbmuxd_socket_path;
    // What made the daemon look installed: its executable or its systemd unit.
    std::string usbmuxd_evidence_path;
    bool project_udev_rule_installed{};
    std::string project_udev_rule_path;
    UsbAccessFinding usb_access;
    std::vector<std::string> user_groups;
    bool member_of_usbmux_group{};
    bool member_of_plugdev_group{};
};

// Reads the whole environment. Every step is read-only: no USB device is
// opened for configuration and no service is started.
[[nodiscard]] Report probe() noexcept;

// Formats the report the way the environment diagnostic string expects. Pure
// function over the report so it can be tested without a live system.
[[nodiscard]] std::wstring describe(const Report& report);

// True when wired capture has everything it needs from the environment. Pure
// function over the report; see describe() for the human-readable reason.
[[nodiscard]] bool wired_capture_ready(const Report& report) noexcept;

} // namespace iPhoneMirror::device::linux_probe
