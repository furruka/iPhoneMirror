// SPDX-License-Identifier: GPL-3.0-only
//
// Linux implementations of the Apple USB device-state queries the shared
// capture state machine asks about. The Windows versions read SetupAPI/cfgmgr32
// PnP evidence; here the answers come from libusb enumeration, which is what
// the Linux port actually has.
//
// Only the queries the shared code paths reach are defined. The rest of
// Device/AppleUsbDiscovery.h describes the libusb-win32 filter stack and its
// PnP interface states, which have no Linux counterpart and are only called
// from Windows-only branches of CaptureSession.

#include "Device/AppleUsbDiscovery.h"

#include "Transport/QtUsbTransport.h"

namespace iPhoneMirror::device {

bool is_apple_usb_parent_present(std::string_view serial) noexcept {
    if (serial.empty()) return false;
    try {
        transport::QtUsbContext context(false);
        transport::AppleUsbIdentity identity;
        identity.serial = serial;
        return context.find_apple_device(identity).has_value();
    } catch (...) {
        // An enumeration failure is not evidence that the device left.
        return false;
    }
}

AppleUsbFilterSafetyResult inspect_apple_usb_filter_stack(
    std::string_view serial) noexcept {
    (void)serial;
    // The unsafe combination this guards against is the libusb-win32 upper
    // filter stacked on Apple's KMDF filters. Linux has no filter driver in
    // the path at all, so the hazard cannot exist here.
    return {
        .safety = AppleUsbFilterSafety::Safe,
        .diagnostic = "linux: libusb talks to the device directly; no Apple USB "
                      "filter driver is involved",
    };
}

} // namespace iPhoneMirror::device
