// SPDX-License-Identifier: GPL-3.0-only
//
// Platform-neutral decoder seam for the capture session. The Windows Media
// Foundation decoder and the future Linux FFmpeg decoder both implement this
// interface, and CaptureSession drives whichever one the platform build
// selected through Media/ActiveVideoDecoder.h. The method set is exactly the
// public surface MediaFoundationVideoDecoder already had, so inheriting it is
// a source-level formality: no call site changes its behaviour.

#pragma once

#include "Media/VideoFormats.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace iPhoneMirror::media {

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual void configure(const coremedia::FormatDescription& format,
        std::uint32_t fps_numerator = 60, std::uint32_t fps_denominator = 1) = 0;
    [[nodiscard]] virtual std::vector<DecodedFrame> decode(
        std::span<const std::uint8_t> length_prefixed_sample,
        std::int64_t timestamp_100ns, std::int64_t duration_100ns) = 0;
    [[nodiscard]] virtual std::vector<DecodedFrame> drain() = 0;
    virtual void flush() = 0;

    [[nodiscard]] virtual DecoderPreference preference() const noexcept = 0;
    [[nodiscard]] virtual std::string_view selected_decoder_name() const noexcept = 0;
    [[nodiscard]] virtual DecoderAcceleration decoder_acceleration() const noexcept = 0;
    [[nodiscard]] virtual bool selected_decoder_is_hardware() const noexcept = 0;
    [[nodiscard]] virtual PixelFormat output_pixel_format() const noexcept = 0;
};

} // namespace iPhoneMirror::media
