// SPDX-License-Identifier: GPL-3.0-only
//
// WP5 acceptance tool for the Linux video decoder. Needs no device: it reads an
// MP4, whose packets are already the length-prefixed AVCC samples a connected
// iPhone sends, and pushes them through the same IVideoDecoder seam
// CaptureSession drives.
//
// Reading the container with libavformat rather than splitting an Annex-B dump
// keeps the input shape honest. The avcC record in the stream's extradata holds
// the parameter sets exactly the way the QuickTime format description does, so
// the FormatDescription this tool builds is the one the capture path builds.

#include "Media/CoreMedia.h"
#include "Media/LinuxFFmpegVideoDecoder.h"
#include "Media/VideoFormats.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace iPhoneMirror;

namespace {

struct FormatContextDeleter {
    void operator()(AVFormatContext* value) const noexcept {
        avformat_close_input(&value);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};

[[nodiscard]] std::uint16_t read_u16(std::span<const std::uint8_t> bytes) noexcept {
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

// ISO/IEC 14496-15 AVCDecoderConfigurationRecord: version, three profile bytes,
// then 6 reserved bits and lengthSizeMinusOne, then 3 reserved bits and the SPS
// count, then length-prefixed SPS and PPS payloads.
[[nodiscard]] bool parse_avcc(std::span<const std::uint8_t> record,
    coremedia::FormatDescription& format) {
    if (record.size() < 7 || record[0] != 1) return false;
    format.nalu_length_size = static_cast<std::uint8_t>((record[4] & 0x03U) + 1U);
    std::size_t offset = 5;
    const auto read_sets = [&](std::size_t count,
        std::vector<std::vector<std::uint8_t>>& sets) {
        for (std::size_t index = 0; index < count; ++index) {
            if (record.size() - offset < 2) return false;
            const auto length = read_u16(record.subspan(offset, 2));
            offset += 2;
            if (record.size() - offset < length) return false;
            const auto payload = record.subspan(offset, length);
            sets.emplace_back(payload.begin(), payload.end());
            offset += length;
        }
        return true;
    };
    const auto sps_count = static_cast<std::size_t>(record[offset++] & 0x1fU);
    if (!read_sets(sps_count, format.sequence_parameter_sets)) return false;
    if (offset >= record.size()) return false;
    const auto pps_count = static_cast<std::size_t>(record[offset++]);
    return read_sets(pps_count, format.picture_parameter_sets);
}

// std::string_view from the name helpers is not guaranteed null-terminated.
#define IM_SV(value) static_cast<int>((value).size()), (value).data()

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: iPhoneMirror.Linux.DecodeProbe <input.mp4> [output.nv12]\n");
        return 2;
    }
    const char* input_path = argv[1];
    const char* output_path = argc > 2 ? argv[2] : nullptr;

    AVFormatContext* raw_context{};
    if (const int result = avformat_open_input(&raw_context, input_path, nullptr,
            nullptr);
        result < 0) {
        std::fprintf(stderr, "cannot open %s\n", input_path);
        return 1;
    }
    std::unique_ptr<AVFormatContext, FormatContextDeleter> container(raw_context);
    if (avformat_find_stream_info(container.get(), nullptr) < 0) {
        std::fprintf(stderr, "cannot read stream info\n");
        return 1;
    }
    const int stream_index = av_find_best_stream(container.get(),
        AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        std::fprintf(stderr, "no video stream\n");
        return 1;
    }
    const AVCodecParameters& parameters =
        *container->streams[stream_index]->codecpar;

    coremedia::FormatDescription format;
    // 'vide' and the codec four-character codes the QuickTime parser reports.
    format.media_type = 0x76696465U;
    format.codec = parameters.codec_id == AV_CODEC_ID_HEVC ? 0x68766331U
                                                           : 0x61766331U;
    format.width = static_cast<std::uint32_t>(parameters.width);
    format.height = static_cast<std::uint32_t>(parameters.height);
    // The QuickTime format description carries the bit depth; the container's
    // pixel format is the equivalent here, and it decides NV12 versus P010.
    if (const auto* pixel = av_pix_fmt_desc_get(
            static_cast<AVPixelFormat>(parameters.format));
        pixel != nullptr && pixel->nb_components > 0) {
        format.bit_depth_luma = static_cast<std::uint8_t>(pixel->comp[0].depth);
        format.bit_depth_chroma = static_cast<std::uint8_t>(
            pixel->nb_components > 1 ? pixel->comp[1].depth : pixel->comp[0].depth);
    }
    if (parameters.extradata != nullptr && parameters.extradata_size > 0 &&
        parameters.codec_id == AV_CODEC_ID_H264) {
        if (!parse_avcc(std::span(parameters.extradata,
                static_cast<std::size_t>(parameters.extradata_size)), format)) {
            std::fprintf(stderr, "cannot parse the avcC record\n");
            return 1;
        }
    }
    std::printf("input                 : %s %ux%u depth=%u nalu_length_size=%u "
                "sps=%zu pps=%zu\n",
        input_path, format.width, format.height, format.bit_depth_luma,
        format.nalu_length_size, format.sequence_parameter_sets.size(),
        format.picture_parameter_sets.size());

    auto decoder = media::make_ffmpeg_video_decoder(
        media::DecoderPreference::SoftwareCompatible);
    try {
        decoder->configure(format, 60, 1);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "configure failed: %s\n", error.what());
        return 1;
    }
    const auto name = decoder->selected_decoder_name();
    std::printf("decoder               : %.*s output=%.*s\n", IM_SV(name),
        IM_SV(media::pixel_format_name(decoder->output_pixel_format())));

    std::FILE* output = output_path != nullptr
        ? std::fopen(output_path, "wb")
        : nullptr;
    if (output_path != nullptr && output == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", output_path);
        return 1;
    }

    std::uint64_t samples{};
    std::uint64_t frames{};
    std::uint64_t written_bytes{};
    std::optional<media::DecodedFrame> first;
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        if (output != nullptr) std::fclose(output);
        std::fprintf(stderr, "av_packet_alloc failed\n");
        return 1;
    }

    const auto consume = [&](std::span<const media::DecodedFrame> decoded) {
        for (const auto& frame : decoded) {
            ++frames;
            if (!first) first = frame;
            if (output != nullptr) {
                written_bytes += std::fwrite(frame.nv12.data(), 1,
                    frame.nv12.size(), output);
            }
        }
    };

    try {
        while (av_read_frame(container.get(), packet.get()) >= 0) {
            if (packet->stream_index == stream_index && packet->size > 0) {
                ++samples;
                // 100 ns units, the same scale the capture path passes down.
                const auto timestamp = static_cast<std::int64_t>(samples - 1) *
                    166667;
                consume(decoder->decode(std::span(packet->data,
                    static_cast<std::size_t>(packet->size)), timestamp, 166667));
            }
            av_packet_unref(packet.get());
        }
        consume(decoder->drain());
    } catch (const std::exception& error) {
        if (output != nullptr) std::fclose(output);
        std::fprintf(stderr, "decode failed after %llu samples: %s\n",
            static_cast<unsigned long long>(samples), error.what());
        return 1;
    }
    if (output != nullptr) std::fclose(output);

    std::printf("samples               : %llu\n",
        static_cast<unsigned long long>(samples));
    std::printf("frames                : %llu\n",
        static_cast<unsigned long long>(frames));
    if (first) {
        std::printf("first frame           : %ux%u stride=%d bytes=%zu "
                    "format=%.*s\n",
            first->width, first->height, first->stride, first->nv12.size(),
            IM_SV(media::pixel_format_name(first->pixel_format)));
        std::printf("colour                : primaries=%.*s transfer=%.*s "
                    "matrix=%.*s range=%.*s hdr=%s\n",
            IM_SV(media::color_primaries_name(first->color.primaries)),
            IM_SV(media::transfer_function_name(first->color.transfer)),
            IM_SV(media::matrix_coefficients_name(first->color.matrix)),
            IM_SV(media::color_range_name(first->color.range)),
            first->color.is_hdr() ? "yes" : "no");
    }
    if (output_path != nullptr) {
        std::printf("wrote                 : %s %llu bytes\n", output_path,
            static_cast<unsigned long long>(written_bytes));
    }
    if (frames == 0) {
        std::fprintf(stderr, "no frames were decoded\n");
        return 1;
    }
    return 0;
}
