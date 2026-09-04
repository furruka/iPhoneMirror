// SPDX-License-Identifier: GPL-3.0-only
//
// Platform-neutral implementations for the decoded-video data model declared
// in VideoFormats.h: format name tables, buffer size checks, the letterboxed
// NV12 copy and the NV12/P010 colour mathematics. Extracted verbatim from
// MediaFoundationDecoder.cpp; on Windows the same code is still compiled into
// iPhoneMirror.Core, so no behaviour changes there.
//
// Two Windows-bound members are deliberately NOT implemented here:
// materialize_gpu_frame() opens D3D11 shared textures, and
// classify_dxva_mode() reads DXVA mode constants from the Windows SDK. Both
// stay in MediaFoundationDecoder.cpp.
//
// DecodedFrame::SharedGpuFrame releases a Windows shared-texture handle; on
// Linux no decoder produces shared GPU frames, so the destructor has nothing
// to release.

#include "Media/VideoFormats.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace iPhoneMirror::media {

std::string_view decoder_preference_name(DecoderPreference value) noexcept {
    switch (value) {
    case DecoderPreference::Auto: return "auto";
    case DecoderPreference::HardwarePreferred: return "hardware_preferred";
    case DecoderPreference::SoftwareCompatible: return "software_compatible";
    }
    return "unknown";
}

std::string_view pixel_format_name(PixelFormat value) noexcept {
    return value == PixelFormat::P010 ? "p010" : "nv12";
}

std::string_view codec_name(coremedia::VideoCodec value) noexcept {
    switch (value) {
    case coremedia::VideoCodec::H264: return "h264";
    case coremedia::VideoCodec::Hevc: return "hevc";
    default: return "unknown";
    }
}

std::string_view color_primaries_name(coremedia::ColorPrimaries value) noexcept {
    switch (value) {
    case coremedia::ColorPrimaries::Bt709: return "bt709";
    case coremedia::ColorPrimaries::Bt2020: return "bt2020";
    case coremedia::ColorPrimaries::DisplayP3: return "display_p3";
    default: return "unspecified";
    }
}

std::string_view transfer_function_name(coremedia::TransferFunction value) noexcept {
    switch (value) {
    case coremedia::TransferFunction::Bt709: return "bt709";
    case coremedia::TransferFunction::Srgb: return "srgb";
    case coremedia::TransferFunction::Pq: return "pq";
    case coremedia::TransferFunction::Hlg: return "hlg";
    default: return "unspecified";
    }
}

std::string_view matrix_coefficients_name(coremedia::MatrixCoefficients value) noexcept {
    switch (value) {
    case coremedia::MatrixCoefficients::Bt601: return "bt601";
    case coremedia::MatrixCoefficients::Bt709: return "bt709";
    case coremedia::MatrixCoefficients::Bt2020: return "bt2020";
    default: return "unspecified";
    }
}

std::string_view color_range_name(coremedia::ColorRange value) noexcept {
    switch (value) {
    case coremedia::ColorRange::Limited: return "limited";
    case coremedia::ColorRange::Full: return "full";
    default: return "unspecified";
    }
}

namespace detail {

std::optional<std::uint32_t> checked_video_buffer_size(
    std::uint32_t width, std::uint32_t height, PixelFormat format) noexcept {
    if (width == 0 || height == 0 || width > MaxDecodedVideoDimension ||
        height > MaxDecodedVideoDimension) return std::nullopt;
    const auto component_bytes = format == PixelFormat::P010 ? 2ULL : 1ULL;
    const auto stride = ((static_cast<std::uint64_t>(width) + 1U) & ~1ULL) * component_bytes;
    const auto y_bytes = stride * height;
    const auto uv_bytes = stride * ((static_cast<std::uint64_t>(height) + 1U) / 2U);
    const auto total = y_bytes + uv_bytes;
    if (total > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(total);
}

std::optional<std::uint32_t> checked_nv12_buffer_size(
    std::uint32_t width, std::uint32_t height) noexcept {
    return checked_video_buffer_size(width, height, PixelFormat::Nv12);
}

bool copy_nv12_frame_letterboxed(const DecodedFrame& frame,
    std::span<std::uint8_t> output, std::uint32_t output_width,
    std::uint32_t output_height) noexcept {
    if (frame.nv12.empty() && frame.gpu_frame) {
        auto materialized = frame;
        if (!materialize_gpu_frame(materialized)) return false;
        return copy_nv12_frame_letterboxed(materialized, output,
            output_width, output_height);
    }
    if (frame.width == 0 || frame.height == 0 || output_width == 0 ||
        output_height == 0 || (output_width & 1U) != 0 ||
        (output_height & 1U) != 0) {
        return false;
    }
    const auto required = checked_nv12_buffer_size(output_width, output_height);
    if (!required || output.size() < *required) return false;

    const auto source_stride = static_cast<std::size_t>(std::abs(frame.stride));
    const auto component_bytes = frame.pixel_format == PixelFormat::P010 ? 2U : 1U;
    const auto source_y_row_bytes = static_cast<std::size_t>(frame.width) *
        component_bytes;
    const auto source_uv_pairs = static_cast<std::size_t>(
        (frame.width + 1U) / 2U);
    const auto source_uv_row_bytes = source_uv_pairs * 2U * component_bytes;
    if (source_stride < std::max(source_y_row_bytes, source_uv_row_bytes))
        return false;

    const auto allocated_height_candidate = source_stride == 0 ? 0U :
        (frame.nv12.size() * 2U) / (source_stride * 3U);
    auto allocated_height = static_cast<std::size_t>(frame.height);
    if (allocated_height_candidate >= frame.height) {
        const auto candidate_required = source_stride * allocated_height_candidate +
            source_stride * ((allocated_height_candidate + 1U) / 2U);
        if (candidate_required <= frame.nv12.size())
            allocated_height = allocated_height_candidate;
    }
    const auto source_y_bytes = source_stride * allocated_height;
    const auto source_required = source_y_bytes + source_stride *
        ((allocated_height + 1U) / 2U);
    if (frame.nv12.size() < source_required) return false;

    const auto output_y_bytes = static_cast<std::size_t>(output_width) *
        output_height;
    const auto black_y = frame.color.range == coremedia::ColorRange::Full
        ? std::uint8_t{0} : std::uint8_t{16};
    std::fill_n(output.data(), output_y_bytes, black_y);
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(output_y_bytes),
        output.begin() + static_cast<std::ptrdiff_t>(*required),
        std::uint8_t{128});

    const auto scale = std::min(1.0, std::min(
        static_cast<double>(output_width) / frame.width,
        static_cast<double>(output_height) / frame.height));
    const auto even_dimension = [](std::uint32_t value,
        std::uint32_t limit) noexcept {
        value = std::min(value, limit);
        if (value < 2U) return std::uint32_t{2};
        return value & ~1U;
    };
    const auto content_width = even_dimension(
        std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(
            std::lround(frame.width * scale))), output_width);
    const auto content_height = even_dimension(
        std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(
            std::lround(frame.height * scale))), output_height);
    const auto left = ((output_width - content_width) / 2U) & ~1U;
    const auto top = ((output_height - content_height) / 2U) & ~1U;
    const auto* source_y = frame.nv12.data();
    const auto* source_uv = source_y + source_y_bytes;
    auto* destination_y = output.data();
    auto* destination_uv = output.data() + output_y_bytes;

    if (frame.pixel_format == PixelFormat::Nv12 &&
        content_width == frame.width && content_height == frame.height) {
        for (std::uint32_t y = 0; y < content_height; ++y) {
            std::memcpy(destination_y + static_cast<std::size_t>(top + y) *
                output_width + left, source_y + static_cast<std::size_t>(y) *
                source_stride, content_width);
        }
        for (std::uint32_t y = 0; y < content_height / 2U; ++y) {
            std::memcpy(destination_uv + static_cast<std::size_t>(top / 2U + y) *
                output_width + left, source_uv + static_cast<std::size_t>(y) *
                source_stride, content_width);
        }
        return true;
    }

    static thread_local std::uint32_t cached_source_width{};
    static thread_local std::uint32_t cached_source_height{};
    static thread_local std::uint32_t cached_content_width{};
    static thread_local std::uint32_t cached_content_height{};
    static thread_local std::vector<std::uint32_t> x_map;
    static thread_local std::vector<std::uint32_t> y_map;
    static thread_local std::vector<std::uint32_t> uv_x_map;
    static thread_local std::vector<std::uint32_t> uv_y_map;
    if (cached_source_width != frame.width ||
        cached_source_height != frame.height ||
        cached_content_width != content_width ||
        cached_content_height != content_height) {
        x_map.resize(content_width);
        y_map.resize(content_height);
        uv_x_map.resize(content_width / 2U);
        uv_y_map.resize(content_height / 2U);
        for (std::uint32_t x = 0; x < content_width; ++x) {
            x_map[x] = std::min<std::uint32_t>(frame.width - 1U,
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) *
                    frame.width / content_width));
        }
        for (std::uint32_t y = 0; y < content_height; ++y) {
            y_map[y] = std::min<std::uint32_t>(frame.height - 1U,
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) *
                    frame.height / content_height));
        }
        const auto source_uv_height = (frame.height + 1U) / 2U;
        for (std::uint32_t x = 0; x < content_width / 2U; ++x) {
            uv_x_map[x] = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(source_uv_pairs - 1U),
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) *
                    source_uv_pairs / (content_width / 2U)));
        }
        for (std::uint32_t y = 0; y < content_height / 2U; ++y) {
            uv_y_map[y] = std::min<std::uint32_t>(source_uv_height - 1U,
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) *
                    source_uv_height / (content_height / 2U)));
        }
        cached_source_width = frame.width;
        cached_source_height = frame.height;
        cached_content_width = content_width;
        cached_content_height = content_height;
    }

    const auto sample_8bit = [&](const std::uint8_t* row,
        std::size_t component) noexcept {
        return frame.pixel_format == PixelFormat::P010
            ? row[component * 2U + 1U] : row[component];
    };
    for (std::uint32_t y = 0; y < content_height; ++y) {
        const auto* source_row = source_y + static_cast<std::size_t>(y_map[y]) *
            source_stride;
        auto* destination_row = destination_y +
            static_cast<std::size_t>(top + y) * output_width + left;
        for (std::uint32_t x = 0; x < content_width; ++x)
            destination_row[x] = sample_8bit(source_row, x_map[x]);
    }
    for (std::uint32_t y = 0; y < content_height / 2U; ++y) {
        const auto* source_row = source_uv +
            static_cast<std::size_t>(uv_y_map[y]) * source_stride;
        auto* destination_row = destination_uv +
            static_cast<std::size_t>(top / 2U + y) * output_width + left;
        for (std::uint32_t x = 0; x < content_width / 2U; ++x) {
            const auto source_component = static_cast<std::size_t>(uv_x_map[x]) * 2U;
            destination_row[x * 2U] = sample_8bit(source_row, source_component);
            destination_row[x * 2U + 1U] = sample_8bit(source_row,
                source_component + 1U);
        }
    }
    return true;
}

std::optional<DxgiReadbackLayout> checked_dxgi_readback_layout(
    std::uint32_t visible_width, std::uint32_t visible_height,
    std::uint32_t allocation_width, std::uint32_t allocation_height,
    std::uint32_t mip_levels, std::uint32_t array_size,
    std::uint32_t source_subresource, std::uint32_t sample_count,
    std::uint32_t row_pitch, PixelFormat format) noexcept {
    if (visible_width == 0 || visible_height == 0 ||
        visible_width > MaxDecodedVideoDimension ||
        visible_height > MaxDecodedVideoDimension ||
        allocation_width < visible_width || allocation_height < visible_height ||
        mip_levels == 0 || array_size == 0 || sample_count != 1 || row_pitch == 0) {
        return std::nullopt;
    }

    const auto maximum_width = static_cast<std::uint64_t>(visible_width) +
        MaxDxgiAllocationPadding;
    const auto maximum_height = static_cast<std::uint64_t>(visible_height) +
        MaxDxgiAllocationPadding;
    if (allocation_width > maximum_width || allocation_height > maximum_height)
        return std::nullopt;

    const auto subresource_count = static_cast<std::uint64_t>(mip_levels) * array_size;
    if (source_subresource >= subresource_count ||
        source_subresource % mip_levels != 0) {
        return std::nullopt;
    }

    const auto component_bytes = format == PixelFormat::P010 ? 2ULL : 1ULL;
    const auto even_allocation_width = static_cast<std::uint64_t>(allocation_width) +
        (allocation_width & 1U);
    const auto minimum_row_pitch = even_allocation_width * component_bytes;
    const auto row_count = static_cast<std::uint64_t>(allocation_height) +
        (static_cast<std::uint64_t>(allocation_height) + 1ULL) / 2ULL;
    if (minimum_row_pitch > std::numeric_limits<std::uint32_t>::max() ||
        row_count > std::numeric_limits<std::uint32_t>::max() ||
        row_pitch < minimum_row_pitch || row_pitch % component_bytes != 0 ||
        row_pitch > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max()) ||
        row_count > MaxDxgiReadbackBytes / row_pitch) {
        return std::nullopt;
    }
    const auto total_bytes = row_count * row_pitch;
    if (total_bytes > MaxDxgiReadbackBytes ||
        total_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return DxgiReadbackLayout{
        .minimum_row_pitch = static_cast<std::uint32_t>(minimum_row_pitch),
        .row_count = static_cast<std::uint32_t>(row_count),
        .total_bytes = static_cast<std::uint32_t>(total_bytes),
    };
}

bool is_random_access_sample(const coremedia::FormatDescription& format,
    std::span<const std::uint8_t> sample) noexcept {
    if (format.nalu_length_size < 1 || format.nalu_length_size > 4) return false;
    std::size_t offset{};
    while (offset < sample.size()) {
        if (sample.size() - offset < format.nalu_length_size) return false;
        std::uint32_t length{};
        for (std::uint8_t index{}; index < format.nalu_length_size; ++index)
            length = (length << 8U) | sample[offset + index];
        offset += format.nalu_length_size;
        if (length == 0 || length > sample.size() - offset) return false;
        if (format.video_codec() == coremedia::VideoCodec::H264) {
            if ((sample[offset] & 0x1fU) == 5U) return true;
        } else if (format.video_codec() == coremedia::VideoCodec::Hevc) {
            const auto type = static_cast<std::uint8_t>((sample[offset] >> 1U) & 0x3fU);
            if (type >= 16U && type <= 21U) return true;
        }
        offset += length;
    }
    return false;
}

YuvConversionParameters yuv_conversion_parameters(PixelFormat format,
    coremedia::ColorRange range, coremedia::MatrixCoefficients matrix) noexcept {
    YuvConversionParameters result;
    const bool full_range = range == coremedia::ColorRange::Full;
    if (format == PixelFormat::P010) {
        constexpr double Denominator = 65535.0;
        result.y_offset = full_range ? 0.0 : (64.0 * 64.0) / Denominator;
        result.y_scale = full_range
            ? Denominator / (1023.0 * 64.0)
            : Denominator / (876.0 * 64.0);
        result.chroma_offset = (512.0 * 64.0) / Denominator;
        result.chroma_scale = full_range
            ? Denominator / (1023.0 * 64.0)
            : Denominator / (896.0 * 64.0);
    } else {
        result.y_offset = full_range ? 0.0 : 16.0 / 255.0;
        result.y_scale = full_range ? 1.0 : 255.0 / 219.0;
        result.chroma_offset = 128.0 / 255.0;
        result.chroma_scale = full_range ? 1.0 : 255.0 / 224.0;
    }
    if (matrix == coremedia::MatrixCoefficients::Bt601) {
        result.red_cr = 1.4020;
        result.green_cb = -0.344136;
        result.green_cr = -0.714136;
        result.blue_cb = 1.7720;
    } else if (matrix == coremedia::MatrixCoefficients::Bt2020) {
        result.red_cr = 1.4746;
        result.green_cb = -0.164553;
        result.green_cr = -0.571353;
        result.blue_cb = 1.8814;
    }
    return result;
}

namespace {

double clamp_unit(double value) noexcept { return std::clamp(value, 0.0, 1.0); }

double inverse_srgb(double value) noexcept {
    value = clamp_unit(value);
    return value <= 0.04045 ? value / 12.92
        : std::pow((value + 0.055) / 1.055, 2.4);
}

double encode_srgb(double value) noexcept {
    value = std::max(0.0, value);
    return value <= 0.0031308 ? value * 12.92
        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

double pq_to_nits(double value) noexcept {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    const auto p = std::pow(clamp_unit(value), 1.0 / m2);
    return 10000.0 * std::pow(std::max((p - c1) /
        std::max(c2 - c3 * p, 1.0e-12), 0.0), 1.0 / m1);
}

double hlg_to_nits(double value, double peak_nits) noexcept {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    value = clamp_unit(value);
    const auto scene = value <= 0.5 ? value * value / 3.0
        : (std::exp((value - c) / a) + b) / 12.0;
    return std::max(peak_nits, 100.0) * std::pow(std::max(scene, 0.0), 1.2);
}

SdrRgb convert_primaries_to_709(SdrRgb value,
    coremedia::ColorPrimaries primaries) noexcept {
    if (primaries == coremedia::ColorPrimaries::Bt2020) {
        return {
            1.6605 * value.red - 0.5876 * value.green - 0.0728 * value.blue,
           -0.1246 * value.red + 1.1329 * value.green - 0.0083 * value.blue,
           -0.0182 * value.red - 0.1006 * value.green + 1.1187 * value.blue,
        };
    }
    if (primaries == coremedia::ColorPrimaries::DisplayP3) {
        return {
            1.224745 * value.red - 0.224904 * value.green,
           -0.042058 * value.red + 1.042081 * value.green,
           -0.019642 * value.red - 0.078655 * value.green + 1.098537 * value.blue,
        };
    }
    return value;
}

double aces_tone_map(double nits) noexcept {
    const auto value = std::max(nits / 203.0, 0.0);
    return clamp_unit((value * (2.51 * value + 0.03)) /
        (value * (2.43 * value + 0.59) + 0.14));
}

} // namespace

SdrRgb convert_yuv_to_sdr(double y, double cb, double cr,
    const coremedia::VideoColorDescription& color, PixelFormat format) noexcept {
    return convert_yuv_to_sdr(y, cb, cr, color,
        yuv_conversion_parameters(format, color.range, color.matrix));
}

SdrRgb convert_yuv_to_sdr(double y, double cb, double cr,
    const coremedia::VideoColorDescription& color,
    const YuvConversionParameters& parameters) noexcept {
    y = std::max(0.0, y - parameters.y_offset) * parameters.y_scale;
    cb = (cb - parameters.chroma_offset) * parameters.chroma_scale;
    cr = (cr - parameters.chroma_offset) * parameters.chroma_scale;
    SdrRgb encoded{
        y + parameters.red_cr * cr,
        y + parameters.green_cb * cb + parameters.green_cr * cr,
        y + parameters.blue_cb * cb,
    };
    encoded.red = clamp_unit(encoded.red);
    encoded.green = clamp_unit(encoded.green);
    encoded.blue = clamp_unit(encoded.blue);
    if (color.is_hdr()) {
        auto peak_nits = color.hdr.max_mastering_luminance != 0
            ? static_cast<double>(color.hdr.max_mastering_luminance)
            : color.hdr.max_content_light_level != 0
            ? static_cast<double>(color.hdr.max_content_light_level) : 1000.0;
        SdrRgb nits;
        if (color.transfer == coremedia::TransferFunction::Pq) {
            nits = {pq_to_nits(encoded.red), pq_to_nits(encoded.green),
                pq_to_nits(encoded.blue)};
        } else {
            nits = {hlg_to_nits(encoded.red, peak_nits),
                hlg_to_nits(encoded.green, peak_nits),
                hlg_to_nits(encoded.blue, peak_nits)};
        }
        nits = convert_primaries_to_709(nits, color.primaries);
        return {
            clamp_unit(encode_srgb(aces_tone_map(nits.red))),
            clamp_unit(encode_srgb(aces_tone_map(nits.green))),
            clamp_unit(encode_srgb(aces_tone_map(nits.blue))),
        };
    }
    if (color.primaries != coremedia::ColorPrimaries::Bt709 &&
        color.primaries != coremedia::ColorPrimaries::Unspecified) {
        auto linear = convert_primaries_to_709({inverse_srgb(encoded.red),
            inverse_srgb(encoded.green), inverse_srgb(encoded.blue)}, color.primaries);
        return {clamp_unit(encode_srgb(linear.red)),
            clamp_unit(encode_srgb(linear.green)),
            clamp_unit(encode_srgb(linear.blue))};
    }
    return encoded;
}

} // namespace detail

DecodedFrame::SharedGpuFrame::~SharedGpuFrame() {
#ifdef _WIN32
    if (shared_handle) {
        CloseHandle(static_cast<HANDLE>(shared_handle));
        shared_handle = nullptr;
    }
#else
    // Shared GPU frames are a Windows decoder concept; nothing to release.
    shared_handle = nullptr;
#endif
}

} // namespace iPhoneMirror::media
