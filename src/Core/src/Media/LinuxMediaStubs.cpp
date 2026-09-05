// SPDX-License-Identifier: GPL-3.0-only
//
// Linux platform backends for the capture session's media seams. The video
// decoder now lives in Media/LinuxFFmpegVideoDecoder.cpp; the PipeWire renderer
// (WP5) replaces the audio stub here. Until then the shared state machine
// compiles on Linux and audio playback does nothing rather than pretending.

#include "Media/ActiveVideoDecoder.h"

#include "Media/CoreMedia.h"
#include "Media/LinuxFFmpegVideoDecoder.h"
#include "Audio/PcmBufferPolicy.h"

#include <stdexcept>

namespace iPhoneMirror::media {

std::unique_ptr<IVideoDecoder> make_platform_video_decoder(
    DecoderPreference preference) {
    return make_ffmpeg_video_decoder(preference);
}

namespace detail {

// The Windows counterpart opens a D3D11 shared texture and reads it back. No
// Linux decoder publishes a cross-device shared GPU frame, so there is never
// anything to materialize; copy_nv12_frame_letterboxed only calls this when
// DecodedFrame::gpu_frame is set.
bool materialize_gpu_frame(DecodedFrame& frame) noexcept {
    return !frame.nv12.empty();
}

} // namespace detail

} // namespace iPhoneMirror::media

namespace iPhoneMirror::audio {

namespace {

class StubAudioRenderer final : public IAudioRenderer {
public:
    void enqueue(std::span<const std::uint8_t>) override {}
    void set_enabled(bool) noexcept override {}
    void set_volume(float) noexcept override {}
    void stop() noexcept override {}
    [[nodiscard]] PlaybackStats stats() const override { return {}; }
};

} // namespace

std::unique_ptr<IAudioRenderer> make_platform_audio_renderer(
    const coremedia::AudioStreamBasicDescription& format,
    bool playback_enabled, float volume) {
    // Reject the same format set the real renderer rejects so the state
    // machine's audio-disable path is exercised identically on both platforms.
    if (!audio::detail::checked_wasapi_buffer_layout(format))
        throw std::invalid_argument("unsupported QuickTime audio format");
    (void)playback_enabled;
    (void)volume;
    return std::make_unique<StubAudioRenderer>();
}

} // namespace iPhoneMirror::audio
