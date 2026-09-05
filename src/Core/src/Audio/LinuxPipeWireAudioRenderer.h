// SPDX-License-Identifier: GPL-3.0-only
//
// Linux implementation of the IAudioRenderer seam, built on PipeWire.
//
// Separate from make_platform_audio_renderer so the renderer can be constructed
// directly by the acceptance tool without going through platform selection.

#pragma once

#include "Audio/IAudioRenderer.h"
#include "Media/CoreMedia.h"

#include <memory>

namespace iPhoneMirror::audio {

// Throws std::invalid_argument when the format is not the interleaved signed
// PCM16 the QuickTime capture path produces, and std::runtime_error when
// PipeWire cannot be reached or the stream cannot be connected. Never returns
// null.
[[nodiscard]] std::unique_ptr<IAudioRenderer> make_pipewire_audio_renderer(
    const coremedia::AudioStreamBasicDescription& format,
    bool playback_enabled, float volume);

} // namespace iPhoneMirror::audio
