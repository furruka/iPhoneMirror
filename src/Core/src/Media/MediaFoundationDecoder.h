// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The platform-neutral decoded-video model lives in VideoFormats.h; this
// header keeps only the Media Foundation decoder itself. Consumers that used
// the names through this header keep compiling unchanged.
#include "Media/VideoFormats.h"

#include <memory>
#include <span>
#include <vector>

namespace iPhoneMirror::media {

class MediaFoundationVideoDecoder {
public:
    explicit MediaFoundationVideoDecoder(
        DecoderPreference preference = DecoderPreference::Auto);
    ~MediaFoundationVideoDecoder();
    MediaFoundationVideoDecoder(const MediaFoundationVideoDecoder&) = delete;
    MediaFoundationVideoDecoder& operator=(const MediaFoundationVideoDecoder&) = delete;

    void configure(const coremedia::FormatDescription& format,
        std::uint32_t fps_numerator = 60, std::uint32_t fps_denominator = 1);
    [[nodiscard]] std::vector<DecodedFrame> decode(
        std::span<const std::uint8_t> length_prefixed_sample,
        std::int64_t timestamp_100ns, std::int64_t duration_100ns);
    [[nodiscard]] std::vector<DecodedFrame> drain();
    void flush();

    [[nodiscard]] DecoderPreference preference() const noexcept;
    [[nodiscard]] std::string_view selected_decoder_name() const noexcept;
    [[nodiscard]] DecoderAcceleration decoder_acceleration() const noexcept;
    [[nodiscard]] bool selected_decoder_is_hardware() const noexcept;
    [[nodiscard]] PixelFormat output_pixel_format() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool com_initialized_{};
};

// Source compatibility for callers compiled against the original H.264-only
// class. configure() now validates and accepts both AVC and HEVC descriptions.
using MediaFoundationH264Decoder = MediaFoundationVideoDecoder;

} // namespace iPhoneMirror::media
