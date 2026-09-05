// SPDX-License-Identifier: GPL-3.0-only
//
// Linux video decoder built on libavcodec, sitting behind the same IVideoDecoder
// seam as the Windows Media Foundation decoder.
//
// Two decisions worth stating, because neither is the library default:
//
// Slice threading, not frame threading. Frame threading buys throughput by
// delaying output several frames, which is exactly the wrong trade for a mirror
// where the whole point is that the screen on the desk and the window on the
// monitor move together. AV_CODEC_FLAG_LOW_DELAY is set for the same reason.
//
// The QuickTime format description stays the colour authority. libavcodec's
// parsed VUI only fills in what the description left Unspecified, and the
// remaining gaps get the same defaults the Windows decoder applies, so a frame
// reaching the renderer carries identical colour metadata on both platforms.

#include "Media/LinuxFFmpegVideoDecoder.h"

#include "Media/H264.h"
#include "Media/VideoFormats.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace iPhoneMirror::media {

namespace {

struct CodecContextDeleter {
    void operator()(AVCodecContext* value) const noexcept {
        avcodec_free_context(&value);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};

struct PacketDeleter {
    void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};

struct SwsDeleter {
    void operator()(SwsContext* value) const noexcept { sws_freeContext(value); }
};

struct HwDeviceDeleter {
    void operator()(AVBufferRef* value) const noexcept { av_buffer_unref(&value); }
};

// True when this decoder advertises a VAAPI hardware configuration. Asking the
// codec is the only reliable check: the build may lack the hwaccel even when the
// platform has a render node.
[[nodiscard]] bool supports_vaapi(const AVCodec& codec) noexcept {
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(&codec, index);
        if (config == nullptr) return false;
        if (config->device_type == AV_HWDEVICE_TYPE_VAAPI &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return true;
        }
    }
}

[[nodiscard]] std::string av_error_text(int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, buffer, sizeof buffer) < 0) return std::format("{}", error);
    return buffer;
}

[[nodiscard]] coremedia::ColorPrimaries map_primaries(AVColorPrimaries value) noexcept {
    switch (value) {
    case AVCOL_PRI_BT709: return coremedia::ColorPrimaries::Bt709;
    case AVCOL_PRI_BT2020: return coremedia::ColorPrimaries::Bt2020;
    // Display P3 shares the SMPTE RP 431/432 primaries; iOS tags its wide-gamut
    // captures this way.
    case AVCOL_PRI_SMPTE431:
    case AVCOL_PRI_SMPTE432: return coremedia::ColorPrimaries::DisplayP3;
    default: return coremedia::ColorPrimaries::Unspecified;
    }
}

[[nodiscard]] coremedia::TransferFunction map_transfer(AVColorTransferCharacteristic value) noexcept {
    switch (value) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12: return coremedia::TransferFunction::Bt709;
    case AVCOL_TRC_IEC61966_2_1: return coremedia::TransferFunction::Srgb;
    case AVCOL_TRC_SMPTE2084: return coremedia::TransferFunction::Pq;
    case AVCOL_TRC_ARIB_STD_B67: return coremedia::TransferFunction::Hlg;
    default: return coremedia::TransferFunction::Unspecified;
    }
}

[[nodiscard]] coremedia::MatrixCoefficients map_matrix(AVColorSpace value) noexcept {
    switch (value) {
    case AVCOL_SPC_BT709: return coremedia::MatrixCoefficients::Bt709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return coremedia::MatrixCoefficients::Bt601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return coremedia::MatrixCoefficients::Bt2020;
    default: return coremedia::MatrixCoefficients::Unspecified;
    }
}

[[nodiscard]] coremedia::ColorRange map_range(AVColorRange value) noexcept {
    switch (value) {
    case AVCOL_RANGE_MPEG: return coremedia::ColorRange::Limited;
    case AVCOL_RANGE_JPEG: return coremedia::ColorRange::Full;
    default: return coremedia::ColorRange::Unspecified;
    }
}

// Same precedence and same fallbacks as the Windows decoder's color_description:
// the format description is the base, anything the bitstream states explicitly
// wins over it, and whatever is still Unspecified gets a deterministic default.
[[nodiscard]] coremedia::VideoColorDescription merge_color_description(
    const coremedia::FormatDescription& format, const AVFrame& frame) noexcept {
    auto color = format.color;
    if (const auto mapped = map_primaries(frame.color_primaries);
        mapped != coremedia::ColorPrimaries::Unspecified) {
        color.primaries = mapped;
    }
    if (const auto mapped = map_transfer(frame.color_trc);
        mapped != coremedia::TransferFunction::Unspecified) {
        color.transfer = mapped;
    }
    if (const auto mapped = map_matrix(frame.colorspace);
        mapped != coremedia::MatrixCoefficients::Unspecified) {
        color.matrix = mapped;
    }
    if (const auto mapped = map_range(frame.color_range);
        mapped != coremedia::ColorRange::Unspecified) {
        color.range = mapped;
    }
    if (color.primaries == coremedia::ColorPrimaries::Unspecified)
        color.primaries = coremedia::ColorPrimaries::Bt709;
    if (color.transfer == coremedia::TransferFunction::Unspecified)
        color.transfer = coremedia::TransferFunction::Bt709;
    if (color.matrix == coremedia::MatrixCoefficients::Unspecified) {
        color.matrix = format.height >= 720
            ? coremedia::MatrixCoefficients::Bt709
            : coremedia::MatrixCoefficients::Bt601;
    }
    if (color.range == coremedia::ColorRange::Unspecified)
        color.range = coremedia::ColorRange::Limited;
    return color;
}

[[nodiscard]] AVCodecID codec_id(coremedia::VideoCodec codec) noexcept {
    switch (codec) {
    case coremedia::VideoCodec::H264: return AV_CODEC_ID_H264;
    case coremedia::VideoCodec::Hevc: return AV_CODEC_ID_HEVC;
    case coremedia::VideoCodec::Unknown: break;
    }
    return AV_CODEC_ID_NONE;
}

// Annex-B extradata: libavcodec's H.264 and HEVC decoders accept a start-code
// delimited parameter set stream, which is what the QuickTime format description
// already gives us as separate NAL payloads. Building an avcC/hvcC record would
// only add a second serialization to get wrong.
[[nodiscard]] std::vector<std::uint8_t> annex_b_parameter_sets(
    const coremedia::FormatDescription& format) {
    std::vector<std::uint8_t> output;
    const auto append = [&output](const std::vector<std::vector<std::uint8_t>>& sets) {
        for (const auto& set : sets) {
            if (set.empty()) continue;
            output.insert(output.end(), {0, 0, 0, 1});
            output.insert(output.end(), set.begin(), set.end());
        }
    };
    append(format.video_parameter_sets);
    append(format.sequence_parameter_sets);
    append(format.picture_parameter_sets);
    return output;
}

class FFmpegVideoDecoder final : public IVideoDecoder {
public:
    explicit FFmpegVideoDecoder(DecoderPreference preference) noexcept
        : preference_(preference) {}

    void configure(const coremedia::FormatDescription& format,
        std::uint32_t fps_numerator, std::uint32_t fps_denominator) override {
        if (!format.is_video())
            throw std::invalid_argument("the decoder was configured with a non-video format");
        const auto id = codec_id(format.video_codec());
        if (id == AV_CODEC_ID_NONE)
            throw std::runtime_error("unsupported QuickTime video codec");
        const AVCodec* codec = avcodec_find_decoder(id);
        if (codec == nullptr) {
            throw std::runtime_error(std::format(
                "libavcodec has no decoder for {}", codec_name(format.video_codec())));
        }

        std::unique_ptr<AVCodecContext, CodecContextDeleter> context(
            avcodec_alloc_context3(codec));
        if (!context) throw std::runtime_error("avcodec_alloc_context3 failed");

        auto extradata = annex_b_parameter_sets(format);
        if (!extradata.empty()) {
            // libavcodec takes ownership of an av_malloc'd buffer and expects
            // AV_INPUT_BUFFER_PADDING_SIZE zeroed bytes past the payload.
            auto* buffer = static_cast<std::uint8_t*>(
                av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (buffer == nullptr) throw std::runtime_error("av_mallocz failed");
            std::memcpy(buffer, extradata.data(), extradata.size());
            context->extradata = buffer;
            context->extradata_size = static_cast<int>(extradata.size());
        }
        context->width = static_cast<int>(format.width);
        context->height = static_cast<int>(format.height);
        context->thread_type = FF_THREAD_SLICE;
        context->thread_count = 0;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (fps_denominator != 0) {
            context->framerate.num = static_cast<int>(fps_numerator);
            context->framerate.den = static_cast<int>(fps_denominator);
        }

        // VAAPI when the caller asked for hardware and the platform can supply
        // it. Every failure below falls through to software rather than throwing:
        // a missing render node or a codec the driver does not implement is a
        // normal state on Linux, not a reason to refuse to decode.
        std::string acceleration_note{"software"};
        auto acceleration = DecoderAcceleration::Software;
        std::unique_ptr<AVBufferRef, HwDeviceDeleter> hw_device;
        if (preference_ != DecoderPreference::SoftwareCompatible &&
            supports_vaapi(*codec)) {
            AVBufferRef* raw_device{};
            const int result = av_hwdevice_ctx_create(&raw_device,
                AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
            if (result < 0) {
                acceleration_note = std::format("software, vaapi unavailable: {}",
                    av_error_text(result));
            } else {
                hw_device.reset(raw_device);
                context->hw_device_ctx = av_buffer_ref(hw_device.get());
                if (context->hw_device_ctx == nullptr)
                    throw std::runtime_error("av_buffer_ref failed");
                context->opaque = this;
                context->get_format = &FFmpegVideoDecoder::select_pixel_format;
                acceleration = DecoderAcceleration::Hardware;
                acceleration_note = "vaapi";
            }
        }

        if (const int result = avcodec_open2(context.get(), codec, nullptr);
            result < 0) {
            throw std::runtime_error(std::format("avcodec_open2 failed: {}",
                av_error_text(result)));
        }

        context_ = std::move(context);
        hw_device_ = std::move(hw_device);
        acceleration_.store(acceleration, std::memory_order_relaxed);
        format_ = format;
        output_format_ = format.bit_depth_luma > 8 || format.bit_depth_chroma > 8
            ? PixelFormat::P010
            : PixelFormat::Nv12;
        decoder_name_ = std::format("{} (libavcodec {}{})", codec->name,
            acceleration_note,
            acceleration == DecoderAcceleration::Hardware ||
                (context_->active_thread_type & FF_THREAD_SLICE) == 0
                ? ""
                : ", slice-threaded");
        scaler_.reset();
        scaler_source_format_ = AV_PIX_FMT_NONE;
        scaler_source_width_ = 0;
        scaler_source_height_ = 0;
    }

    [[nodiscard]] std::vector<DecodedFrame> decode(
        std::span<const std::uint8_t> length_prefixed_sample,
        std::int64_t timestamp_100ns, std::int64_t duration_100ns) override {
        require_configured();
        // The device sends AVCC/HVCC samples; both are the same length-prefixed
        // container, so the H.264 helper converts either one.
        auto annex_b = h264::avcc_to_annex_b(length_prefixed_sample,
            format_.nalu_length_size);
        if (annex_b.empty()) return {};

        std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
        if (!packet) throw std::runtime_error("av_packet_alloc failed");
        if (const int result = av_new_packet(packet.get(),
                static_cast<int>(annex_b.size()));
            result < 0) {
            throw std::runtime_error(std::format("av_new_packet failed: {}",
                av_error_text(result)));
        }
        std::memcpy(packet->data, annex_b.data(), annex_b.size());
        // No time base is set on the context, so libavcodec passes these through
        // untouched and the 100 ns units survive reordering.
        packet->pts = timestamp_100ns;
        packet->dts = timestamp_100ns;
        packet->duration = duration_100ns;

        if (const int result = avcodec_send_packet(context_.get(), packet.get());
            result < 0 && result != AVERROR(EAGAIN)) {
            throw std::runtime_error(std::format("avcodec_send_packet failed: {}",
                av_error_text(result)));
        }
        return receive_frames(timestamp_100ns);
    }

    [[nodiscard]] std::vector<DecodedFrame> drain() override {
        if (!context_) return {};
        if (const int result = avcodec_send_packet(context_.get(), nullptr);
            result < 0 && result != AVERROR_EOF) {
            throw std::runtime_error(std::format(
                "avcodec_send_packet(flush) failed: {}", av_error_text(result)));
        }
        return receive_frames(0);
    }

    void flush() override {
        if (context_) avcodec_flush_buffers(context_.get());
    }

    [[nodiscard]] DecoderPreference preference() const noexcept override {
        return preference_;
    }
    [[nodiscard]] std::string_view selected_decoder_name() const noexcept override {
        return decoder_name_;
    }
    [[nodiscard]] DecoderAcceleration decoder_acceleration() const noexcept override {
        return acceleration_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool selected_decoder_is_hardware() const noexcept override {
        return acceleration_.load(std::memory_order_relaxed) ==
            DecoderAcceleration::Hardware;
    }
    [[nodiscard]] PixelFormat output_pixel_format() const noexcept override {
        return output_format_;
    }

private:
    // libavcodec asks which of the formats it can produce we want. Picking
    // AV_PIX_FMT_VAAPI keeps the frame on the GPU; anything else means the
    // hwaccel did not initialize and the software path is what we get.
    static AVPixelFormat select_pixel_format(AVCodecContext* context,
        const AVPixelFormat* formats) noexcept {
        for (const AVPixelFormat* candidate = formats;
            *candidate != AV_PIX_FMT_NONE; ++candidate) {
            if (*candidate == AV_PIX_FMT_VAAPI) return *candidate;
        }
        if (context != nullptr && context->opaque != nullptr) {
            static_cast<FFmpegVideoDecoder*>(context->opaque)->acceleration_.store(
                DecoderAcceleration::Software, std::memory_order_relaxed);
        }
        return formats[0];
    }

    // A VAAPI frame lives in GPU memory, so it has to come down before the
    // packing conversion can touch it. Anything else is already in system
    // memory and is returned unchanged.
    [[nodiscard]] const AVFrame* download(const AVFrame& source,
        AVFrame& destination) {
        if (source.format != AV_PIX_FMT_VAAPI) return &source;
        if (const int result = av_hwframe_transfer_data(&destination, &source, 0);
            result < 0) {
            throw std::runtime_error(std::format(
                "av_hwframe_transfer_data failed: {}", av_error_text(result)));
        }
        // The transfer copies pixels, not metadata, and convert() reads both.
        if (const int result = av_frame_copy_props(&destination, &source);
            result < 0) {
            throw std::runtime_error(std::format("av_frame_copy_props failed: {}",
                av_error_text(result)));
        }
        return &destination;
    }

    void require_configured() const {
        if (!context_)
            throw std::logic_error("the decoder was used before configure()");
    }

    [[nodiscard]] std::vector<DecodedFrame> receive_frames(
        std::int64_t fallback_timestamp_100ns) {
        std::vector<DecodedFrame> frames;
        std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
        if (!frame) throw std::runtime_error("av_frame_alloc failed");
        std::unique_ptr<AVFrame, FrameDeleter> software(av_frame_alloc());
        if (!software) throw std::runtime_error("av_frame_alloc failed");
        for (;;) {
            const int result = avcodec_receive_frame(context_.get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) {
                throw std::runtime_error(std::format(
                    "avcodec_receive_frame failed: {}", av_error_text(result)));
            }
            frames.push_back(convert(*download(*frame, *software),
                fallback_timestamp_100ns));
            av_frame_unref(frame.get());
            av_frame_unref(software.get());        }
        return frames;
    }

    // Converts one decoded frame into the tightly packed semi-planar buffer the
    // seam promises. Odd dimensions are rounded down: 4:2:0 chroma cannot address
    // a half pixel, and every Apple capture geometry measured so far is even, so
    // this only ever guards against a malformed stream.
    [[nodiscard]] DecodedFrame convert(const AVFrame& source,
        std::int64_t fallback_timestamp_100ns) {
        const auto width = static_cast<std::uint32_t>(std::max(source.width, 0)) & ~1U;
        const auto height = static_cast<std::uint32_t>(std::max(source.height, 0)) & ~1U;
        const auto size = detail::checked_video_buffer_size(width, height,
            output_format_);
        if (!size) {
            throw std::runtime_error(std::format(
                "decoded frame geometry {}x{} is not a usable {} buffer",
                width, height, pixel_format_name(output_format_)));
        }

        DecodedFrame frame;
        frame.width = width;
        frame.height = height;
        frame.pixel_format = output_format_;
        frame.stride = static_cast<std::int32_t>(
            output_format_ == PixelFormat::P010 ? width * 2U : width);
        frame.timestamp_100ns = source.pts == AV_NOPTS_VALUE
            ? fallback_timestamp_100ns
            : source.pts;
        frame.received_at = std::chrono::steady_clock::now();
        frame.color = merge_color_description(format_, source);
        frame.nv12.resize(*size);

        const auto destination_format = output_format_ == PixelFormat::P010
            ? AV_PIX_FMT_P010LE
            : AV_PIX_FMT_NV12;
        const auto source_format = static_cast<AVPixelFormat>(source.format);
        if (!scaler_ || scaler_source_format_ != source_format ||
            scaler_source_width_ != static_cast<int>(width) ||
            scaler_source_height_ != static_cast<int>(height)) {
            scaler_.reset(sws_getContext(static_cast<int>(width),
                static_cast<int>(height), source_format, static_cast<int>(width),
                static_cast<int>(height), destination_format, SWS_POINT, nullptr,
                nullptr, nullptr));
            if (!scaler_) {
                throw std::runtime_error(std::format(
                    "sws_getContext failed for {} -> {}",
                    av_get_pix_fmt_name(source_format) != nullptr
                        ? av_get_pix_fmt_name(source_format) : "?",
                    pixel_format_name(output_format_)));
            }
            scaler_source_format_ = source_format;
            scaler_source_width_ = static_cast<int>(width);
            scaler_source_height_ = static_cast<int>(height);
        }

        const int luma_stride = frame.stride;
        std::uint8_t* planes[]{frame.nv12.data(),
            frame.nv12.data() + static_cast<std::size_t>(luma_stride) * height,
            nullptr, nullptr};
        const int strides[]{luma_stride, luma_stride, 0, 0};
        const int converted = sws_scale(scaler_.get(), source.data,
            source.linesize, 0, static_cast<int>(height), planes, strides);
        if (converted != static_cast<int>(height)) {
            throw std::runtime_error(std::format(
                "sws_scale produced {} of {} rows", converted, height));
        }
        return frame;
    }


    DecoderPreference preference_;
    std::unique_ptr<AVCodecContext, CodecContextDeleter> context_;
    std::unique_ptr<AVBufferRef, HwDeviceDeleter> hw_device_;
    // Written by the get_format callback on the decode thread and read by the
    // capture session's stats, so it is atomic rather than a plain enum.
    std::atomic<DecoderAcceleration> acceleration_{DecoderAcceleration::Software};
    std::unique_ptr<SwsContext, SwsDeleter> scaler_;
    AVPixelFormat scaler_source_format_{AV_PIX_FMT_NONE};
    int scaler_source_width_{};
    int scaler_source_height_{};
    coremedia::FormatDescription format_;
    PixelFormat output_format_{PixelFormat::Nv12};
    std::string decoder_name_{"(libavcodec, unconfigured)"};
};

} // namespace

std::unique_ptr<IVideoDecoder> make_ffmpeg_video_decoder(
    DecoderPreference preference) {
    return std::make_unique<FFmpegVideoDecoder>(preference);
}

} // namespace iPhoneMirror::media
