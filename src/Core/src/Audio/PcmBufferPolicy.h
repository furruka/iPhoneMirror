// SPDX-License-Identifier: GPL-3.0-only
//
// Platform-neutral PCM ring buffer and queue policy shared by the Windows
// WASAPI renderer and the future Linux PipeWire renderer. Extracted verbatim
// from WasapiRenderer.{h,cpp}: the threshold arithmetic never touches a
// platform API, and the 'lpcm' format constants describe Apple's QuickTime
// wire format rather than any OS audio stack.

#pragma once

#include "Media/CoreMedia.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace iPhoneMirror::audio::detail {

// 'lpcm' format flags from Apple's QuickTime screen-capture wire format.
inline constexpr std::uint32_t LinearPcm = 0x6c70636dU; // 'lpcm'
inline constexpr std::uint32_t PcmIsFloat = 1U << 0U;
inline constexpr std::uint32_t PcmIsBigEndian = 1U << 1U;
inline constexpr std::uint32_t PcmIsSignedInteger = 1U << 2U;
inline constexpr std::uint32_t PcmIsNonInterleaved = 1U << 5U;

struct WasapiBufferLayout {
    std::uint32_t block_align{};
    std::size_t capacity_frames{};
    std::size_t capacity_bytes{};
};

struct WasapiQueueThresholds {
    std::size_t startup_frames{};
    std::size_t high_water_frames{};
};

struct WasapiEnqueuePlan {
    std::size_t drop_existing_frames{};
    std::size_t final_frames{};
};

[[nodiscard]] std::optional<WasapiBufferLayout> checked_wasapi_buffer_layout(
    const coremedia::AudioStreamBasicDescription& format,
    std::size_t minimum_capacity_frames = 0) noexcept;
[[nodiscard]] WasapiQueueThresholds wasapi_queue_thresholds(
    std::size_t maximum_packet_frames, std::size_t capacity_frames,
    std::size_t endpoint_buffer_frames = 0,
    std::size_t base_startup_frames = 3072,
    std::size_t base_high_water_frames = 4096) noexcept;
[[nodiscard]] WasapiEnqueuePlan plan_wasapi_enqueue(
    std::size_t queued_frames, std::size_t incoming_frames,
    std::size_t capacity_frames, WasapiQueueThresholds thresholds) noexcept;

} // namespace iPhoneMirror::audio::detail
