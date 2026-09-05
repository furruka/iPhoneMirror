// SPDX-License-Identifier: GPL-3.0-only
//
// Tests for the Linux re-enumeration recovery policy. This is the WP4 design
// with the highest risk attached to it (see docs/LINUX_PORT.md: udev writes
// bConfigurationValue back to 0 after every add event), so the behaviour is
// pinned here rather than discovered on real hardware.

#include "Capture/UsbReenumerationPolicy.h"

#include <iostream>
#include <string_view>

namespace {

using namespace iPhoneMirror::capture::detail;

int failures{};

void check(bool condition, std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

constexpr std::uint8_t QuickTimeConfiguration = 6;

} // namespace

int main() {
    // Classification precedence.
    check(classify_reenumeration_state(false, true, true, QuickTimeConfiguration,
              QuickTimeConfiguration) == ReenumerationObservation::Missing,
        "an absent device is Missing regardless of its last known state");
    check(classify_reenumeration_state(true, false, true, 4,
              QuickTimeConfiguration) ==
              ReenumerationObservation::PresentWithoutQuickTimeDescriptor,
        "no QuickTime descriptor outranks the active configuration");
    check(classify_reenumeration_state(true, true, true, 0,
              QuickTimeConfiguration) ==
              ReenumerationObservation::PresentUnconfigured,
        "configuration 0 is the unconfigured state udev leaves behind");
    check(classify_reenumeration_state(true, true, false, QuickTimeConfiguration,
              QuickTimeConfiguration) ==
              ReenumerationObservation::PresentUnconfigured,
        "an unreadable active configuration is not treated as success");
    check(classify_reenumeration_state(true, true, true, QuickTimeConfiguration,
              QuickTimeConfiguration) ==
              ReenumerationObservation::PresentQuickTime,
        "the expected configuration is PresentQuickTime");
    check(classify_reenumeration_state(true, true, true, 4,
              QuickTimeConfiguration) ==
              ReenumerationObservation::PresentOtherConfiguration,
        "the normal configuration is PresentOtherConfiguration");

    // The happy path still requires stability: one sample is not enough,
    // because udev may not have processed the add event yet.
    {
        UsbReenumerationPolicy policy;
        check(policy.observe(ReenumerationObservation::PresentQuickTime) ==
                  ReenumerationAction::Wait,
            "a single QuickTime sample does not authorize a claim");
        check(policy.observe(ReenumerationObservation::PresentQuickTime) ==
                  ReenumerationAction::Claim,
            "two consecutive QuickTime samples authorize a claim");
        check(policy.finished(), "the policy is finished after claiming");
        check(policy.observe(ReenumerationObservation::PresentUnconfigured) ==
                  ReenumerationAction::Claim,
            "a finished policy keeps reporting Claim and does not restart");
    }

    // The udev case: present but unconfigured, so the host sets the
    // configuration, and the value sticks on the second attempt.
    {
        UsbReenumerationPolicy policy;
        policy.note_quicktime_descriptor_present();
        check(policy.observe(ReenumerationObservation::Missing) ==
                  ReenumerationAction::Wait,
            "in-flight re-enumeration only waits");
        check(policy.observe(ReenumerationObservation::PresentUnconfigured) ==
                  ReenumerationAction::SetQuickTimeConfiguration,
            "an unconfigured device triggers SET_CONFIGURATION");
        check(policy.observe(ReenumerationObservation::PresentQuickTime) ==
                  ReenumerationAction::Wait,
            "the first success still waits for stability");
        check(policy.observe(ReenumerationObservation::PresentQuickTime) ==
                  ReenumerationAction::Claim,
            "stability reached after the configuration was set");
        check(policy.configuration_attempts() == 1,
            "exactly one SET_CONFIGURATION was issued");
        check(policy.configuration_overwrites() == 0,
            "no overwrite is counted when the value was never held first");
    }

    // udev overwriting a configuration that was already active must be counted:
    // that count is the evidence for the hypothesis in docs/LINUX_PORT.md.
    {
        UsbReenumerationPolicy policy;
        policy.note_quicktime_descriptor_present();
        (void)policy.observe(ReenumerationObservation::PresentQuickTime);
        check(policy.observe(ReenumerationObservation::PresentUnconfigured) ==
                  ReenumerationAction::SetQuickTimeConfiguration,
            "losing the configuration triggers another SET_CONFIGURATION");
        check(policy.configuration_overwrites() == 1,
            "the overwrite is counted");
        (void)policy.observe(ReenumerationObservation::PresentQuickTime);
        check(policy.observe(ReenumerationObservation::PresentQuickTime) ==
                  ReenumerationAction::Claim,
            "recovery after an overwrite still reaches a claim");
    }

    // The normal configuration before the switch has taken effect is not
    // something to correct: re-issuing SET_CONFIGURATION there would fight the
    // device rather than udev.
    {
        UsbReenumerationPolicy policy;
        check(policy.observe(ReenumerationObservation::PresentOtherConfiguration) ==
                  ReenumerationAction::Wait,
            "the pre-switch configuration is waited out, not corrected");
        check(policy.configuration_attempts() == 0,
            "no SET_CONFIGURATION is issued before the switch is seen");
        policy.note_quicktime_descriptor_present();
        check(policy.observe(ReenumerationObservation::PresentOtherConfiguration) ==
                  ReenumerationAction::SetQuickTimeConfiguration,
            "once the descriptor is known the wrong configuration is corrected");
    }

    // A device exposing no QuickTime descriptor cannot be corrected by any
    // request; only re-enumeration changes that.
    {
        UsbReenumerationPolicy policy;
        for (int index = 0; index < 10; ++index) {
            check(policy.observe(
                      ReenumerationObservation::PresentWithoutQuickTimeDescriptor) ==
                      ReenumerationAction::Wait,
                "a missing descriptor never triggers a request");
        }
        check(policy.configuration_attempts() == 0,
            "no attempts are spent on a device without the descriptor");
    }

    // A device that keeps losing the configuration must terminate rather than
    // loop forever, and must not report success.
    {
        UsbReenumerationPolicy policy;
        policy.note_quicktime_descriptor_present();
        std::uint32_t requests{};
        ReenumerationAction action{ReenumerationAction::Wait};
        for (int index = 0; index < 50; ++index) {
            action = policy.observe(ReenumerationObservation::PresentUnconfigured);
            if (action == ReenumerationAction::SetQuickTimeConfiguration) ++requests;
            if (action == ReenumerationAction::GiveUp) break;
        }
        check(action == ReenumerationAction::GiveUp,
            "a device that never keeps the configuration ends in GiveUp");
        check(requests == UsbReenumerationPolicy::MaxConfigurationAttempts,
            "the attempt budget is exactly the documented bound");
        check(policy.finished(), "the policy is finished after giving up");
        check(policy.observe(ReenumerationObservation::PresentQuickTime) ==
                  ReenumerationAction::GiveUp,
            "a policy that gave up does not later claim success");
    }

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "UsbReenumerationPolicyTests: all checks passed\n";
    return 0;
}
