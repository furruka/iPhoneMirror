// SPDX-License-Identifier: GPL-3.0-only
//
// Platform selection for the capture session's media backends. CaptureSession
// holds unique_ptr<media::IVideoDecoder> and unique_ptr<audio::IAudioRenderer>
// and asks this factory to build the concrete implementation; which one that
// is depends only on the platform being compiled.
//
// Windows: Media Foundation + WASAPI, unchanged.
// Linux: stubs for now. The FFmpeg decoder and PipeWire renderer replace them
// in later work packages; the stubs keep the shared state machine compilable
// and honest about being unimplemented.

#pragma once

#include "Audio/IAudioRenderer.h"
#include "Media/IVideoDecoder.h"

#include <memory>

namespace iPhoneMirror::media {

[[nodiscard]] std::unique_ptr<IVideoDecoder> make_platform_video_decoder(
    DecoderPreference preference);

} // namespace iPhoneMirror::media

namespace iPhoneMirror::audio {

[[nodiscard]] std::unique_ptr<IAudioRenderer> make_platform_audio_renderer(
    const coremedia::AudioStreamBasicDescription& format,
    bool playback_enabled, float volume);

} // namespace iPhoneMirror::audio
