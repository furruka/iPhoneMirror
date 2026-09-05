// SPDX-License-Identifier: GPL-3.0-only
//
// WP5 acceptance tool for the Linux preview renderer. Needs no device and no
// window: it reads a raw NV12 frame, renders it through libplacebo, reads the
// result back and compares it against this project's own CPU colour maths.
//
// The input is deliberately the output of LinuxDecodeProbe, which was already
// verified byte-for-byte against ffmpeg. That keeps this tool honest about what
// it is testing: the GPU colour pipeline, not the decoder.

#include "Media/LinuxPlaceboRenderer.h"
#include "Media/VideoFormats.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

using namespace iPhoneMirror;

namespace {

[[nodiscard]] bool read_file(const char* path, std::size_t bytes,
    std::vector<std::uint8_t>& destination) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    destination.resize(bytes);
    const auto read = std::fread(destination.data(), 1, bytes, file);
    std::fclose(file);
    return read == bytes;
}

[[nodiscard]] bool write_ppm(const char* path, std::span<const std::uint8_t> rgba,
    std::uint32_t width, std::uint32_t height) {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) return false;
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height;
        ++pixel) {
        std::fwrite(rgba.data() + pixel * 4U, 1, 3, file);
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: iPhoneMirror.Linux.RenderProbe "
                             "<input.nv12> <width> <height> [output.ppm]\n");
        return 2;
    }
    const char* input_path = argv[1];
    const auto width = static_cast<std::uint32_t>(std::atoi(argv[2]));
    const auto height = static_cast<std::uint32_t>(std::atoi(argv[3]));
    const char* output_path = argc > 4 ? argv[4] : nullptr;
    if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0) {
        std::fprintf(stderr, "the geometry must be even and non-zero\n");
        return 2;
    }

    media::DecodedFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = static_cast<std::int32_t>(width);
    frame.pixel_format = media::PixelFormat::Nv12;
    // The same defaults the decoder applies when the stream leaves them
    // unspecified, so the CPU and GPU paths are told the same thing.
    frame.color.primaries = coremedia::ColorPrimaries::Bt709;
    frame.color.transfer = coremedia::TransferFunction::Bt709;
    frame.color.matrix = height >= 720 ? coremedia::MatrixCoefficients::Bt709
                                       : coremedia::MatrixCoefficients::Bt601;
    frame.color.range = coremedia::ColorRange::Limited;

    const auto frame_bytes = static_cast<std::size_t>(width) * height * 3U / 2U;
    if (!read_file(input_path, frame_bytes, frame.nv12)) {
        std::fprintf(stderr, "cannot read %zu bytes from %s\n", frame_bytes,
            input_path);
        return 1;
    }

    std::unique_ptr<media::ILinuxPreviewRenderer> renderer;
    try {
        renderer = media::make_placebo_preview_renderer(width, height);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "renderer construction failed: %s\n", error.what());
        return 1;
    }
    std::printf("device                : %.*s\n",
        static_cast<int>(renderer->device_name().size()),
        renderer->device_name().data());
    std::printf("target                : %ux%u\n", renderer->target_width(),
        renderer->target_height());

    const auto surface = renderer->exported_surface();
    if (surface.valid) {
        std::printf("exported surface      : memory_fd=%d done_fd=%d free_fd=%d "
                    "vk_format=%u size=%llu offset=%llu\n",
            surface.memory_fd, surface.render_completed_fd, surface.available_fd,
            surface.vk_format,
            static_cast<unsigned long long>(surface.allocation_size),
            static_cast<unsigned long long>(surface.allocation_offset));
    } else {
        std::printf("exported surface      : unavailable on this platform\n");
    }

    if (!renderer->present(frame)) {
        std::fprintf(stderr, "present failed: %.*s\n",
            static_cast<int>(renderer->last_error().size()),
            renderer->last_error().data());
        return 1;
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U);
    if (!renderer->read_back_rgba(rgba)) {
        std::fprintf(stderr, "readback failed: %.*s\n",
            static_cast<int>(renderer->last_error().size()),
            renderer->last_error().data());
        return 1;
    }

    // Compare against the CPU path. Chroma is upsampled differently on the two
    // sides — libplacebo interpolates, the reference samples the co-sited pair —
    // so exact equality is not the bar. Large deviation would mean the colour
    // description was mis-plumbed, which is what this checks.
    const auto* luma = frame.nv12.data();
    const auto* chroma = frame.nv12.data() +
        static_cast<std::size_t>(width) * height;
    double total{};
    int worst{};
    std::size_t worst_x{};
    std::size_t worst_y{};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto chroma_offset = static_cast<std::size_t>(y / 2) * width +
                static_cast<std::size_t>(x / 2) * 2U;
            const auto reference = media::detail::convert_yuv_to_sdr(
                luma[static_cast<std::size_t>(y) * width + x] / 255.0,
                chroma[chroma_offset] / 255.0,
                chroma[chroma_offset + 1] / 255.0, frame.color,
                media::PixelFormat::Nv12);
            const auto* actual = rgba.data() +
                (static_cast<std::size_t>(y) * width + x) * 4U;
            const double expected[3]{reference.red, reference.green,
                reference.blue};
            for (int channel = 0; channel < 3; ++channel) {
                const auto want = static_cast<int>(std::lround(
                    std::clamp(expected[channel], 0.0, 1.0) * 255.0));
                const auto difference = std::abs(
                    static_cast<int>(actual[channel]) - want);
                total += difference;
                if (difference > worst) {
                    worst = difference;
                    worst_x = x;
                    worst_y = y;
                }
            }
        }
    }
    const auto samples = static_cast<double>(width) * height * 3.0;
    std::printf("vs CPU colour maths   : mean=%.3f worst=%d at (%zu,%zu)\n",
        total / samples, worst, worst_x, worst_y);

    if (output_path != nullptr) {
        if (!write_ppm(output_path, rgba, width, height)) {
            std::fprintf(stderr, "cannot write %s\n", output_path);
            return 1;
        }
        std::printf("wrote                 : %s\n", output_path);
    }

    // A mean above one channel step would mean the two paths disagree about the
    // colour description rather than merely about chroma interpolation.
    if (total / samples > 1.0) {
        std::fprintf(stderr, "the GPU and CPU colour paths disagree\n");
        return 1;
    }
    std::printf("verdict               : PASS\n");
    return 0;
}
