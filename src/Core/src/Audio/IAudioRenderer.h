// SPDX-License-Identifier: GPL-3.0-only
//
// Platform-neutral audio-renderer seam for the capture session. The Windows
// WASAPI renderer and the future Linux PipeWire renderer both implement this
// interface; CaptureSession only ever sees the seam. The method set mirrors
// WasapiRenderer's existing public surface.

#pragma once

#include <cstdint>
#include <span>

namespace iPhoneMirror::audio {

struct PlaybackStats {
    bool active{};
    std::uint64_t queued_frames{};
    std::uint64_t rendered_frames{};
    std::uint64_t dropped_frames{};
    std::uint64_t underruns{};
};

class IAudioRenderer {
public:
    virtual ~IAudioRenderer() = default;

    virtual void enqueue(std::span<const std::uint8_t> pcm) = 0;
    virtual void set_enabled(bool enabled) noexcept = 0;
    virtual void set_volume(float volume) noexcept = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual PlaybackStats stats() const = 0;
};

} // namespace iPhoneMirror::audio
