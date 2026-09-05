// SPDX-License-Identifier: GPL-3.0-only
//
// Linux platform backends for the capture session's media seams. The FFmpeg
// decoder (WP5) and the PipeWire renderer (WP5) replace these stubs; until
// then the shared state machine compiles on Linux and every construction
// reports an explicit, greppable "not implemented yet" failure rather than
// silently doing nothing.

#include "Media/ActiveVideoDecoder.h"

#include "Media/CoreMedia.h"
#include "Audio/PcmBufferPolicy.h"

#include <stdexcept>

namespace iPhoneMirror::media {

namespace {

class StubVideoDecoder final : public IVideoDecoder {
public:
    explicit StubVideoDecoder(DecoderPreference preference)
        : preference_(preference) {}

    void configure(const coremedia::FormatDescription&,
        std::uint32_t, std::uint32_t) override {
        fail();
    }
    [[nodiscard]] std::vector<DecodedFrame> decode(
        std::span<const std::uint8_t>, std::int64_t, std::int64_t) override {
        fail();
        return {};
    }
    [[nodiscard]] std::vector<DecodedFrame> drain() override { return {}; }
    void flush() override {}

    [[nodiscard]] DecoderPreference preference() const noexcept override {
        return preference_;
    }
    [[nodiscard]] std::string_view selected_decoder_name() const noexcept override {
        return "(linux stub)";
    }
    [[nodiscard]] DecoderAcceleration decoder_acceleration() const noexcept override {
        return DecoderAcceleration::Unknown;
    }
    [[nodiscard]] bool selected_decoder_is_hardware() const noexcept override {
        return false;
    }
    [[nodiscard]] PixelFormat output_pixel_format() const noexcept override {
        return PixelFormat::Nv12;
    }

private:
    [[noreturn]] static void fail() {
        throw std::runtime_error(
            "the Linux FFmpeg video decoder is not implemented yet (WP5)");
    }

    DecoderPreference preference_;
};

} // namespace

std::unique_ptr<IVideoDecoder> make_platform_video_decoder(
    DecoderPreference preference) {
    return std::make_unique<StubVideoDecoder>(preference);
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
