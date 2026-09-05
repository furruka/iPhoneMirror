// SPDX-License-Identifier: GPL-3.0-only
//
// Recovering the QuickTime USB configuration after re-enumeration on Linux.
//
// Windows does not need this. There, AppleUsbFilter leaves the configuration
// the 0x52 vendor request selected in place. On Linux usbmuxd ships
// /usr/lib/udev/rules.d/39-usbmuxd.rules, which contains:
//
//   ACTION=="add", ENV{USBMUX_SUPPORTED}="1", ATTR{bConfigurationValue}="0", ...
//
// so every time the device re-appears udev writes the active configuration back
// to 0, leaving it unconfigured. libusb_claim_interface then fails because no
// configuration is selected. The host has to notice this and re-issue
// SET_CONFIGURATION for the QuickTime value itself.
//
// The race is the reason this is a state machine rather than a single call: udev
// processes the add event asynchronously, so a configuration set too early can
// be overwritten right afterwards. The policy therefore requires the QuickTime
// configuration to be observed as stable across consecutive samples before it
// lets the caller claim, bounds how many times it will re-issue the request, and
// counts the overwrites so the caller can log what udev actually did.

#pragma once

#include <cstdint>

namespace iPhoneMirror::capture::detail {

enum class ReenumerationObservation {
    // The device is not enumerated right now. Normal while re-enumeration is
    // in flight; only a timeout makes it a failure.
    Missing,
    // Enumerated with the QuickTime configuration descriptor present, but the
    // active configuration is 0. This is the state udev leaves behind.
    PresentUnconfigured,
    // Enumerated with the QuickTime configuration active.
    PresentQuickTime,
    // Enumerated with some other configuration active, typically the normal
    // management one, meaning the QuickTime switch has not taken effect yet.
    PresentOtherConfiguration,
    // Enumerated but exposing no QuickTime configuration descriptor at all.
    PresentWithoutQuickTimeDescriptor,
};

enum class ReenumerationAction {
    Wait,
    // Issue SET_CONFIGURATION for the expected QuickTime configuration value.
    SetQuickTimeConfiguration,
    // The QuickTime configuration has been active across enough consecutive
    // samples to claim the interface.
    Claim,
    // The configuration could not be made to stick. The caller must not retry
    // blindly; the device is left for a cable re-plug to reset.
    GiveUp,
};

class UsbReenumerationPolicy final {
public:
    // Consecutive samples with the QuickTime configuration active before the
    // interface may be claimed. Two samples means one udev processing window
    // has passed without the value being written back to 0.
    static constexpr std::uint32_t StableSamplesBeforeClaim = 2;
    // How many times SET_CONFIGURATION is re-issued before giving up. Each
    // attempt costs one settle interval, so this bounds the whole recovery.
    static constexpr std::uint32_t MaxConfigurationAttempts = 5;

    [[nodiscard]] ReenumerationAction observe(
        ReenumerationObservation observation) noexcept {
        if (finished_) {
            return claimed_ ? ReenumerationAction::Claim
                            : ReenumerationAction::GiveUp;
        }

        switch (observation) {
        case ReenumerationObservation::PresentQuickTime:
            ++stable_quicktime_samples_;
            if (stable_quicktime_samples_ >= StableSamplesBeforeClaim) {
                finished_ = true;
                claimed_ = true;
                return ReenumerationAction::Claim;
            }
            return ReenumerationAction::Wait;

        case ReenumerationObservation::PresentUnconfigured:
            // Losing the configuration after having had it is udev writing
            // bConfigurationValue back to 0. Count it: that count is the
            // evidence for how this device actually behaves.
            if (stable_quicktime_samples_ != 0) ++configuration_overwrites_;
            stable_quicktime_samples_ = 0;
            return request_configuration();

        case ReenumerationObservation::PresentOtherConfiguration:
            stable_quicktime_samples_ = 0;
            // Before the switch has been seen to take effect at all, the normal
            // configuration is simply the pre-switch state and re-issuing
            // SET_CONFIGURATION would fight the device rather than udev.
            if (!quicktime_seen_) return ReenumerationAction::Wait;
            return request_configuration();

        case ReenumerationObservation::PresentWithoutQuickTimeDescriptor:
            // The hidden configuration is not exposed yet. Only re-enumeration
            // can change that, so there is nothing to request.
            stable_quicktime_samples_ = 0;
            return ReenumerationAction::Wait;

        case ReenumerationObservation::Missing:
            stable_quicktime_samples_ = 0;
            return ReenumerationAction::Wait;
        }
        return ReenumerationAction::Wait;
    }

    // Called by the caller once it has observed the QuickTime descriptor, which
    // is what makes a non-QuickTime active configuration worth correcting.
    void note_quicktime_descriptor_present() noexcept { quicktime_seen_ = true; }

    [[nodiscard]] std::uint32_t configuration_attempts() const noexcept {
        return configuration_attempts_;
    }
    // How many times the configuration was lost after having been set. A
    // nonzero value on Linux is udev's 39-usbmuxd.rules acting on the device.
    [[nodiscard]] std::uint32_t configuration_overwrites() const noexcept {
        return configuration_overwrites_;
    }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

private:
    [[nodiscard]] ReenumerationAction request_configuration() noexcept {
        if (configuration_attempts_ >= MaxConfigurationAttempts) {
            finished_ = true;
            claimed_ = false;
            return ReenumerationAction::GiveUp;
        }
        ++configuration_attempts_;
        return ReenumerationAction::SetQuickTimeConfiguration;
    }

    bool quicktime_seen_{};
    bool finished_{};
    bool claimed_{};
    std::uint32_t stable_quicktime_samples_{};
    std::uint32_t configuration_attempts_{};
    std::uint32_t configuration_overwrites_{};
};

// Classifies a sampled device state into the observation the policy expects.
// Kept as a free function so the same rule is used by the capture path and by
// the tests, and so the caller never has to reimplement the precedence.
[[nodiscard]] inline ReenumerationObservation classify_reenumeration_state(
    bool present, bool quicktime_descriptor_present,
    bool active_configuration_known, std::uint8_t active_configuration,
    std::uint8_t expected_quicktime_configuration) noexcept {
    if (!present) return ReenumerationObservation::Missing;
    if (!quicktime_descriptor_present)
        return ReenumerationObservation::PresentWithoutQuickTimeDescriptor;
    // An unreadable active configuration is treated as unconfigured rather than
    // as success: claiming on an unknown configuration is what fails with
    // LIBUSB_ERROR_NOT_FOUND after udev has reset it.
    if (!active_configuration_known)
        return ReenumerationObservation::PresentUnconfigured;
    if (active_configuration == 0)
        return ReenumerationObservation::PresentUnconfigured;
    if (expected_quicktime_configuration != 0 &&
        active_configuration == expected_quicktime_configuration)
        return ReenumerationObservation::PresentQuickTime;
    return ReenumerationObservation::PresentOtherConfiguration;
}

} // namespace iPhoneMirror::capture::detail
