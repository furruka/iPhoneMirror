// SPDX-License-Identifier: GPL-3.0-only
//
// Tests for the pure decision functions in the Linux environment probe. The
// probe() call itself reads the live system, so what is testable is the part
// that turns findings into a readiness verdict and a diagnostic: that is also
// the part that decides what the user is told to fix.

#include "Device/LinuxEnvironmentProbe.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace iPhoneMirror::device::linux_probe;

int failures{};

void check(bool condition, std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

Report ready_report() {
    Report report;
    report.usbmuxd = UsbmuxdAvailability::Connected;
    report.usbmuxd_socket_path = "/var/run/usbmuxd";
    report.project_udev_rule_installed = true;
    report.project_udev_rule_path = "/etc/udev/rules.d/70-iphonemirror.rules";
    report.usb_access.access = UsbAccess::Granted;
    report.usb_access.apple_nodes_seen = 1;
    report.usb_access.apple_nodes_openable = 1;
    return report;
}

bool mentions(const std::wstring& text, std::wstring_view fragment) {
    return text.find(fragment) != std::wstring::npos;
}

} // namespace

int main() {
    // A fully prepared system with an openable device is ready.
    check(wired_capture_ready(ready_report()), "granted access is ready");

    // usbmuxd must be installed regardless of device-node access.
    auto no_usbmuxd = ready_report();
    no_usbmuxd.usbmuxd = UsbmuxdAvailability::NotInstalled;
    check(!wired_capture_ready(no_usbmuxd), "absent usbmuxd is not ready");
    no_usbmuxd.usbmuxd = UsbmuxdAvailability::SocketPresentNotAccepting;
    check(!wired_capture_ready(no_usbmuxd),
        "a socket that refuses connections is not ready");

    // udev starts usbmuxd when a device is attached, so an idle system with no
    // device and no socket is still ready. This is the state the acceptance
    // tool runs in, and calling it a fault would be wrong.
    auto idle = ready_report();
    idle.usbmuxd = UsbmuxdAvailability::InstalledNotRunning;
    idle.usbmuxd_evidence_path = "/usr/lib/systemd/system/usbmuxd.service";
    idle.usb_access.access = UsbAccess::Unknown;
    idle.usb_access.apple_nodes_seen = 0;
    idle.usb_access.apple_nodes_openable = 0;
    check(wired_capture_ready(idle),
        "installed-but-idle usbmuxd with the rule installed is ready");
    const auto idle_text = describe(idle);
    check(mentions(idle_text, L"正常状态"),
        "the idle diagnostic says the state is normal");
    check(!mentions(idle_text, L"请安装 usbmuxd"),
        "the idle diagnostic does not tell the user to install usbmuxd");

    // A device that is attached and openable still needs usbmuxd running for
    // discovery to return its serial.
    auto attached_without_daemon = ready_report();
    attached_without_daemon.usbmuxd = UsbmuxdAvailability::InstalledNotRunning;
    check(!wired_capture_ready(attached_without_daemon),
        "an attached device with usbmuxd down is not ready");

    // A device that cannot be opened is the udev case and must not be ready
    // even though the rule file appears to be installed: the rule may not have
    // been reloaded, or the user may not be in the granted group yet.
    auto denied = ready_report();
    denied.usb_access.access = UsbAccess::DeniedNeedsUdevRule;
    denied.usb_access.apple_nodes_seen = 1;
    denied.usb_access.apple_nodes_openable = 0;
    check(!wired_capture_ready(denied), "denied device node is not ready");

    // With no device attached the verdict rests on the rule being installed.
    auto no_device = ready_report();
    no_device.usb_access.access = UsbAccess::Unknown;
    no_device.usb_access.apple_nodes_seen = 0;
    no_device.usb_access.apple_nodes_openable = 0;
    check(wired_capture_ready(no_device),
        "no device with the rule installed is ready");
    no_device.project_udev_rule_installed = false;
    check(!wired_capture_ready(no_device),
        "no device without the rule is not ready");

    // Root access must not be reported as though the rule were installed: the
    // next non-root run would fail with no explanation.
    auto as_root = ready_report();
    as_root.project_udev_rule_installed = false;
    as_root.usb_access.access = UsbAccess::GrantedAsRoot;
    check(wired_capture_ready(as_root), "root can open the device node");
    const auto root_text = describe(as_root);
    check(mentions(root_text, L"root"), "the root case is named in the diagnostic");
    check(mentions(root_text, L"udev"),
        "the root case still tells the user to install the udev rule");

    // The denied case must name udev and usbmux so the fix is discoverable.
    const auto denied_text = describe(denied);
    check(mentions(denied_text, L"udev"), "denied diagnostic names udev");
    check(mentions(denied_text, L"usbmux"), "denied diagnostic names usbmux");

    // The not-installed case must name usbmuxd and tell the user to install it.
    auto absent = ready_report();
    absent.usbmuxd = UsbmuxdAvailability::NotInstalled;
    const auto absent_text = describe(absent);
    check(mentions(absent_text, L"usbmuxd"), "diagnostic names usbmuxd");
    check(mentions(absent_text, L"请安装 usbmuxd"),
        "the absent case tells the user to install usbmuxd");

    // A ready system should not tell the user to install anything.
    const auto ready_text = describe(ready_report());
    check(!mentions(ready_text, L"未安装"),
        "the ready diagnostic reports no missing rule");

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "LinuxEnvironmentProbeTests: all checks passed\n";
    return 0;
}
