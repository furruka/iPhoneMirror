// SPDX-License-Identifier: GPL-3.0-only
//
// Platform-neutral implementations for the PCM ring buffer and queue policy
// declared in PcmBufferPolicy.h. Extracted verbatim from WasapiRenderer.cpp.

#include "Audio/PcmBufferPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace iPhoneMirror::audio::detail {

std::optional<WasapiBufferLayout> checked_wasapi_buffer_layout(
    const coremedia::AudioStreamBasicDescription& format,
    std::size_t minimum_capacity_frames) noexcept {
    const bool pcm16_interleaved = format.format_id == LinearPcm &&
        (format.format_flags & PcmIsFloat) == 0 &&
        (format.format_flags & PcmIsBigEndian) == 0 &&
        (format.format_flags & PcmIsSignedInteger) != 0 &&
        (format.format_flags & PcmIsNonInterleaved) == 0 &&
        std::isfinite(format.sample_rate) &&
        format.sample_rate >= 8000.0 && format.sample_rate <= 192000.0 &&
        format.channels_per_frame >= 1 && format.channels_per_frame <= 8 &&
        format.bits_per_channel == 16 &&
        format.bytes_per_frame == format.channels_per_frame * 2U;
    if (!pcm16_interleaved) return std::nullopt;

    const auto frames = std::max({std::size_t{8192}, minimum_capacity_frames,
        static_cast<std::size_t>(format.sample_rate / 6.0)});
    const auto align = static_cast<std::size_t>(format.bytes_per_frame);
    if (frames > std::numeric_limits<std::size_t>::max() / align) {
        return std::nullopt;
    }
    return WasapiBufferLayout{
        .block_align = format.bytes_per_frame,
        .capacity_frames = frames,
        .capacity_bytes = frames * align,
    };
}

WasapiQueueThresholds wasapi_queue_thresholds(
    std::size_t maximum_packet_frames, std::size_t capacity_frames,
    std::size_t endpoint_buffer_frames, std::size_t base_startup_frames,
    std::size_t base_high_water_frames) noexcept {
    if (capacity_frames == 0) return {};
    const auto packet = std::min(maximum_packet_frames, capacity_frames);
    const auto endpoint = std::min(endpoint_buffer_frames, capacity_frames);
    const auto capped_add = [capacity_frames](std::size_t left,
                                std::size_t right) noexcept {
        if (left >= capacity_frames || right >= capacity_frames - left)
            return capacity_frames;
        return left + right;
    };

    auto startup = std::min(base_startup_frames, capacity_frames);
    startup = std::max(startup, packet);
    if (endpoint != 0) startup = std::max(startup, capped_add(packet, endpoint));

    auto high_water = std::min(base_high_water_frames, capacity_frames);
    high_water = std::max(high_water, capped_add(startup, packet));
    return {
        .startup_frames = startup,
        .high_water_frames = std::max(startup, high_water),
    };
}

WasapiEnqueuePlan plan_wasapi_enqueue(
    std::size_t queued_frames, std::size_t incoming_frames,
    std::size_t capacity_frames, WasapiQueueThresholds thresholds) noexcept {
    if (capacity_frames == 0) return {};
    auto queued = std::min(queued_frames, capacity_frames);
    const auto incoming = std::min(incoming_frames, capacity_frames);
    const auto startup = std::min(thresholds.startup_frames, capacity_frames);
    const auto high_water = std::max({startup, incoming,
        std::min(thresholds.high_water_frames, capacity_frames)});
    std::size_t dropped{};

    const auto permitted_existing = high_water - incoming;
    if (queued > permitted_existing) {
        dropped = queued - permitted_existing;
        queued = permitted_existing;
    }
    return {
        .drop_existing_frames = dropped,
        .final_frames = queued + incoming,
    };
}

} // namespace iPhoneMirror::audio::detail
