// SPDX-License-Identifier: GPL-3.0-only
//
// Linux port spike S4: FFmpeg decode to libplacebo render, headless.
//
// The Windows preview path is Media Foundation -> shared NV12 texture ->
// D3D11 shader conversion -> DirectComposition. This spike measures the
// proposed Linux equivalent, FFmpeg -> (VAAPI surface or NV12 in host memory)
// -> libplacebo Vulkan renderer, and reports per-stage timings so the
// architecture decision rests on measured numbers rather than expectation.
//
// Rendering is offscreen into a pl_tex, so the probe needs no window, no
// compositor and no display server. It answers three questions:
//   1. Does hardware decode produce frames libplacebo can map?
//   2. Is the zero-copy hardware path actually taken, or does it fall back?
//   3. What is the per-frame decode and render cost at mirroring resolutions?

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

// The FFmpeg glue implementation is emitted by PlaceboAvGlue.c, which is the
// only translation unit allowed to define PL_LIBAV_IMPLEMENTATION as 1.
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

struct Stats {
    std::vector<double> samples;

    void add(double value) { samples.push_back(value); }

    [[nodiscard]] double mean() const {
        if (samples.empty()) return 0.0;
        double total = 0.0;
        for (const auto value : samples) total += value;
        return total / static_cast<double>(samples.size());
    }

    [[nodiscard]] double percentile(double fraction) const {
        if (samples.empty()) return 0.0;
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>(sorted.size() - 1) + 0.5);
        return sorted[std::min(index, sorted.size() - 1)];
    }

    [[nodiscard]] double max() const {
        if (samples.empty()) return 0.0;
        return *std::max_element(samples.begin(), samples.end());
    }
};

void report(const char* label, const Stats& stats) {
    std::printf("  %-22s mean=%7.3f ms  p95=%7.3f ms  max=%7.3f ms  n=%zu\n",
        label, stats.mean(), stats.percentile(0.95), stats.max(),
        stats.samples.size());
}

AVPixelFormat requested_hardware_format = AV_PIX_FMT_NONE;

AVPixelFormat select_hardware_format(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
        if (*format == requested_hardware_format) return *format;
    return AV_PIX_FMT_NONE;
}

struct Options {
    std::string input;
    std::string device{"/dev/dri/renderD128"};
    std::string vulkan_device;
    bool software{};
    bool verbose{};
    // Allocates the render target with an exportable opaque FD handle, which is
    // what the Avalonia compositor requires to import it (see spike S3). The
    // decode and render path must keep working under that constraint, so it is
    // measured rather than assumed.
    bool export_target{};
    int frame_limit{600};
};

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (argument == "--software") {
            options.software = true;
        } else if (argument == "--export-target") {
            options.export_target = true;
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--device") {
            const char* value = next();
            if (value == nullptr) return false;
            options.device = value;
        } else if (argument == "--vulkan-device") {
            const char* value = next();
            if (value == nullptr) return false;
            options.vulkan_device = value;
        } else if (argument == "--frames") {
            const char* value = next();
            if (value == nullptr) return false;
            options.frame_limit = std::atoi(value);
        } else if (!argument.empty() && argument.front() == '-') {
            return false;
        } else {
            options.input = argument;
        }
    }
    return !options.input.empty();
}

void log_libplacebo(void*, pl_log_level, const char* message) {
    std::fprintf(stderr, "libplacebo: %s\n", message);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: %s <input.h264|input.mp4> [--software] "
            "[--device /dev/dri/renderD128] [--vulkan-device NAME] "
            "[--export-target] [--frames N] [--verbose]\n", argv[0]);
        return 64;
    }

    std::printf("Linux port spike S4: FFmpeg decode to libplacebo render (headless)\n");
    std::printf("input         : %s\n", options.input.c_str());
    std::printf("decode mode   : %s\n",
        options.software ? "software" : "VAAPI hardware");
    if (!options.software)
        std::printf("render device : %s\n", options.device.c_str());

    AVFormatContext* format_context{};
    if (avformat_open_input(&format_context, options.input.c_str(), nullptr,
            nullptr) < 0) {
        std::fprintf(stderr, "cannot open input\n");
        return 2;
    }
    if (avformat_find_stream_info(format_context, nullptr) < 0) {
        std::fprintf(stderr, "cannot read stream info\n");
        avformat_close_input(&format_context);
        return 2;
    }

    const AVCodec* codec{};
    const int stream_index = av_find_best_stream(format_context,
        AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream_index < 0 || codec == nullptr) {
        std::fprintf(stderr, "no video stream\n");
        avformat_close_input(&format_context);
        return 2;
    }

    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    if (decoder == nullptr) {
        avformat_close_input(&format_context);
        return 2;
    }
    avcodec_parameters_to_context(decoder,
        format_context->streams[stream_index]->codecpar);

    AVBufferRef* hardware_device{};
    if (!options.software) {
        if (av_hwdevice_ctx_create(&hardware_device, AV_HWDEVICE_TYPE_VAAPI,
                options.device.c_str(), nullptr, 0) < 0) {
            std::fprintf(stderr,
                "VAAPI device unavailable; rerun with --software\n");
            avcodec_free_context(&decoder);
            avformat_close_input(&format_context);
            return 3;
        }
        decoder->hw_device_ctx = av_buffer_ref(hardware_device);
        requested_hardware_format = AV_PIX_FMT_VAAPI;
        decoder->get_format = select_hardware_format;
    }

    if (avcodec_open2(decoder, codec, nullptr) < 0) {
        std::fprintf(stderr, "cannot open decoder\n");
        avcodec_free_context(&decoder);
        av_buffer_unref(&hardware_device);
        avformat_close_input(&format_context);
        return 3;
    }
    std::printf("decoder       : %s\n", codec->name);

    // libplacebo's pl_*_params() convenience macros expand to C compound
    // literals, which C++ cannot take the address of. Declare the parameter
    // structs explicitly instead.
    pl_log_params log_params{};
    log_params.log_cb = log_libplacebo;
    log_params.log_level = options.verbose ? PL_LOG_DEBUG : PL_LOG_WARN;
    pl_log log = pl_log_create(PL_API_VER, &log_params);

    pl_vulkan_params vulkan_params = pl_vulkan_default_params;
    if (!options.vulkan_device.empty())
        vulkan_params.device_name = options.vulkan_device.c_str();
    pl_vulkan vulkan = pl_vulkan_create(log, &vulkan_params);
    if (vulkan == nullptr) {
        std::fprintf(stderr, "cannot create a Vulkan device\n");
        return 4;
    }
    pl_gpu gpu = vulkan->gpu;
    std::printf("vulkan api    : %u.%u.%u\n",
        VK_API_VERSION_MAJOR(vulkan->api_version),
        VK_API_VERSION_MINOR(vulkan->api_version),
        VK_API_VERSION_PATCH(vulkan->api_version));
    std::printf("dmabuf import : %s\n",
        (gpu->import_caps.tex & PL_HANDLE_DMA_BUF) != 0 ? "supported"
                                                        : "unsupported");

    pl_renderer renderer = pl_renderer_create(log, gpu);
    if (renderer == nullptr) {
        std::fprintf(stderr, "cannot create the libplacebo renderer\n");
        return 4;
    }

    pl_tex target_texture{};
    // pl_map_avframe_ex requires a caller-owned texture array for software
    // formats and reuses it across frames to avoid per-frame allocations.
    // Hardware surfaces leave the entries untouched.
    pl_tex plane_textures[4]{};
    Stats decode_stats;
    Stats map_stats;
    Stats render_stats;
    int rendered_frames = 0;
    int mapped_failures = 0;
    std::string observed_format = "unknown";
    int frame_width = 0;
    int frame_height = 0;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto wall_start = Clock::now();

    const auto drain_decoder = [&]() {
        while (rendered_frames < options.frame_limit) {
            const auto decode_start = Clock::now();
            const int result = avcodec_receive_frame(decoder, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            if (result < 0) return;
            decode_stats.add(milliseconds(Clock::now() - decode_start));

            if (frame_width == 0) {
                frame_width = frame->width;
                frame_height = frame->height;
                const char* name = av_get_pix_fmt_name(
                    static_cast<AVPixelFormat>(frame->format));
                observed_format = name != nullptr ? name : "unknown";
            }

            if (target_texture == nullptr) {
                pl_tex_params texture_params{};
                texture_params.w = frame->width;
                texture_params.h = frame->height;
                texture_params.format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
                    PL_FMT_CAP_RENDERABLE);
                texture_params.renderable = true;
                if (options.export_target) {
                    texture_params.sampleable = true;
                    texture_params.blit_src = true;
                    texture_params.blit_dst = true;
                    texture_params.export_handle = PL_HANDLE_FD;
                }
                target_texture = pl_tex_create(gpu, &texture_params);
                if (target_texture == nullptr) {
                    std::fprintf(stderr, "cannot allocate the render target\n");
                    av_frame_unref(frame);
                    return;
                }
                if (options.export_target)
                    std::printf("export target : fd=%d size=%zu offset=%zu\n",
                        target_texture->shared_mem.handle.fd,
                        static_cast<size_t>(target_texture->shared_mem.size),
                        static_cast<size_t>(target_texture->shared_mem.offset));
            }

            pl_frame image{};
            pl_avframe_params map_params{};
            map_params.frame = frame;
            map_params.tex = plane_textures;
            map_params.map_dovi = true;
            const auto map_start = Clock::now();
            const bool mapped = pl_map_avframe_ex(gpu, &image, &map_params);
            map_stats.add(milliseconds(Clock::now() - map_start));
            if (!mapped) {
                ++mapped_failures;
                av_frame_unref(frame);
                continue;
            }

            pl_frame target{};
            target.num_planes = 1;
            target.planes[0].texture = target_texture;
            target.planes[0].components = 3;
            target.planes[0].component_mapping[0] = 0;
            target.planes[0].component_mapping[1] = 1;
            target.planes[0].component_mapping[2] = 2;
            target.repr.sys = PL_COLOR_SYSTEM_RGB;
            target.repr.levels = PL_COLOR_LEVELS_FULL;
            target.color = pl_color_space_srgb;
            target.crop = pl_rect2df{0, 0, static_cast<float>(frame->width),
                static_cast<float>(frame->height)};

            const auto render_start = Clock::now();
            const bool ok = pl_render_image(renderer, &image, &target,
                &pl_render_default_params);
            pl_gpu_finish(gpu);
            render_stats.add(milliseconds(Clock::now() - render_start));
            if (ok) ++rendered_frames;

            pl_unmap_avframe(gpu, &image);
            av_frame_unref(frame);
        }
    };

    while (rendered_frames < options.frame_limit &&
        av_read_frame(format_context, packet) >= 0) {
        if (packet->stream_index == stream_index) {
            if (avcodec_send_packet(decoder, packet) >= 0) drain_decoder();
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(decoder, nullptr);
    drain_decoder();

    const auto wall_total = milliseconds(Clock::now() - wall_start);

    std::printf("\nresults\n");
    std::printf("  frame size           : %dx%d\n", frame_width, frame_height);
    std::printf("  decoded pixel format : %s%s\n", observed_format.c_str(),
        observed_format == "vaapi" ? "  (zero-copy hardware surface)" : "");
    std::printf("  frames rendered      : %d\n", rendered_frames);
    if (mapped_failures != 0)
        std::printf("  map failures         : %d\n", mapped_failures);
    report("decode receive", decode_stats);
    report("gpu map", map_stats);
    report("render + finish", render_stats);
    const double per_frame = decode_stats.mean() + map_stats.mean() +
        render_stats.mean();
    std::printf("  per-frame pipeline   : %.3f ms  (%.1f fps ceiling)\n",
        per_frame, per_frame > 0.0 ? 1000.0 / per_frame : 0.0);
    std::printf("  wall clock           : %.1f ms for %d frames\n", wall_total,
        rendered_frames);

    if (target_texture != nullptr) pl_tex_destroy(gpu, &target_texture);
    for (auto& plane_texture : plane_textures)
        if (plane_texture != nullptr) pl_tex_destroy(gpu, &plane_texture);
    pl_renderer_destroy(&renderer);
    pl_vulkan_destroy(&vulkan);
    pl_log_destroy(&log);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    av_buffer_unref(&hardware_device);
    avformat_close_input(&format_context);

    return rendered_frames > 0 ? 0 : 5;
}
