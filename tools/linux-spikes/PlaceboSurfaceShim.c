// SPDX-License-Identifier: GPL-3.0-only
//
// Linux port spike S3/S4: native producer for the Avalonia preview surface.
// See PlaceboSurfaceShim.h for the contract and for why the decode device is
// derived rather than configured.

#include "PlaceboSurfaceShim.h"

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

// This translation unit owns libplacebo's FFmpeg glue. It is the only place
// allowed to define PL_LIBAV_IMPLEMENTATION as 1, and it must be compiled as C
// because libav_internal.h refuses to build from C++.
#define PL_LIBAV_IMPLEMENTATION 1
#include <libplacebo/utils/libav.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Avalonia's ImportedImage always transitions the imported image to
// TRANSFER_SRC_OPTIMAL and blits out of it, so that is the layout the producer
// must hand over and expect back.
#define PMS_SHARED_LAYOUT VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL

struct pms_context {
    pl_log log;
    pl_vulkan vulkan;
    pl_renderer renderer;
    pl_tex texture;

    // Caller-owned plane storage that pl_map_avframe_ex requires for software
    // pixel formats and reuses across frames. Hardware surfaces leave it alone.
    pl_tex plane_textures[4];

    VkSemaphore render_completed;
    VkSemaphore available;
    union pl_handle render_completed_handle;
    union pl_handle available_handle;

    AVFormatContext *format_context;
    AVCodecContext *decoder;
    AVBufferRef *hardware_device;
    AVPacket *packet;
    AVFrame *frame;
    int stream_index;
    int32_t loop;
    int32_t stream_ended;
    int32_t has_pending_frame;

    // Playback clock. Frames are held back until their presentation timestamp
    // is due, otherwise a 165 Hz compositor would race through a 60 fps stream.
    double clock_origin_ms;
    double first_frame_seconds;
    double fallback_frame_interval_seconds;
    int64_t presented_frames;

    uint32_t queue_family;
    int32_t verbose;
    int32_t zero_copy;
    int32_t video_width;
    int32_t video_height;
    int64_t map_failures;

    // The texture starts out owned by libplacebo, so the first frame must not
    // wait on `available`: nothing has signalled it yet.
    int32_t held_by_consumer;
    int32_t described;
    int64_t frames_rendered;

    char device_name[256];
    char render_node[64];
    char decoder_name[64];
    char error[512];
};

static double pms_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double) now.tv_sec * 1000.0 + (double) now.tv_nsec / 1.0e6;
}

static void pms_log_callback(void *user_data, enum pl_log_level level,
                             const char *message) {
    (void) user_data;
    (void) level;
    fprintf(stderr, "libplacebo: %s\n", message);
}

static void pms_set_error(pms_context *context, const char *message) {
    snprintf(context->error, sizeof context->error, "%s", message);
}

static void pms_set_av_error(pms_context *context, const char *what, int code) {
    char reason[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, reason, sizeof reason);
    snprintf(context->error, sizeof context->error, "%s: %s", what, reason);
}

static void pms_close_handle(union pl_handle *handle) {
    if (handle->fd >= 0) {
        close(handle->fd);
        handle->fd = -1;
    }
}

// Resolves the DRM render node that backs the Vulkan physical device libplacebo
// selected. Getting this from the driver rather than guessing matters: on a
// hybrid laptop the integrated GPU is not necessarily renderD128.
static void pms_resolve_render_node(pms_context *context) {
    context->render_node[0] = '\0';

    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateDeviceExtensionProperties) context->vulkan->get_proc_addr(
            context->vulkan->instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceProperties2) context->vulkan->get_proc_addr(
            context->vulkan->instance, "vkGetPhysicalDeviceProperties2");
    if (enumerate_extensions == NULL || get_properties == NULL)
        return;

    uint32_t count = 0;
    if (enumerate_extensions(context->vulkan->phys_device, NULL, &count,
            NULL) != VK_SUCCESS || count == 0)
        return;

    VkExtensionProperties *extensions = calloc(count, sizeof *extensions);
    if (extensions == NULL)
        return;
    bool supported = false;
    if (enumerate_extensions(context->vulkan->phys_device, NULL, &count,
            extensions) == VK_SUCCESS) {
        for (uint32_t index = 0; index < count; index++) {
            if (strcmp(extensions[index].extensionName,
                    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0) {
                supported = true;
                break;
            }
        }
    }
    free(extensions);
    if (!supported)
        return;

    VkPhysicalDeviceDrmPropertiesEXT drm_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drm_properties,
    };
    get_properties(context->vulkan->phys_device, &properties);
    if (drm_properties.hasRender)
        snprintf(context->render_node, sizeof context->render_node,
            "/dev/dri/renderD%lld", (long long) drm_properties.renderMinor);
}

static enum AVPixelFormat pms_select_vaapi_format(AVCodecContext *decoder,
                                                  const enum AVPixelFormat *formats) {
    (void) decoder;
    for (const enum AVPixelFormat *format = formats;
         *format != AV_PIX_FMT_NONE; format++) {
        if (*format == AV_PIX_FMT_VAAPI)
            return *format;
    }
    return AV_PIX_FMT_NONE;
}

static bool pms_open_input(pms_context *context, const struct pms_config *config) {
    int result = avformat_open_input(&context->format_context, config->input_path,
        NULL, NULL);
    if (result < 0) {
        pms_set_av_error(context, "cannot open the input", result);
        return false;
    }
    result = avformat_find_stream_info(context->format_context, NULL);
    if (result < 0) {
        pms_set_av_error(context, "cannot read stream information", result);
        return false;
    }

    const AVCodec *codec = NULL;
    context->stream_index = av_find_best_stream(context->format_context,
        AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (context->stream_index < 0 || codec == NULL) {
        pms_set_error(context, "the input has no decodable video stream");
        return false;
    }

    context->decoder = avcodec_alloc_context3(codec);
    if (context->decoder == NULL) {
        pms_set_error(context, "cannot allocate the decoder");
        return false;
    }
    result = avcodec_parameters_to_context(context->decoder,
        context->format_context->streams[context->stream_index]->codecpar);
    if (result < 0) {
        pms_set_av_error(context, "cannot configure the decoder", result);
        return false;
    }

    // VAAPI is attempted on the render node that belongs to the render device.
    // A failure here is not fatal: software decode still produces frames the
    // compositor can display, which is the documented fallback for hardware
    // whose DRM format modifiers libplacebo cannot import.
    if (!config->force_software_decode && context->render_node[0] != '\0') {
        result = av_hwdevice_ctx_create(&context->hardware_device,
            AV_HWDEVICE_TYPE_VAAPI, context->render_node, NULL, 0);
        if (result >= 0) {
            context->decoder->hw_device_ctx =
                av_buffer_ref(context->hardware_device);
            context->decoder->get_format = pms_select_vaapi_format;
        } else if (config->verbose) {
            fprintf(stderr,
                "shim: VAAPI unavailable on %s, decoding in software\n",
                context->render_node);
        }
    }

    result = avcodec_open2(context->decoder, codec, NULL);
    if (result < 0) {
        pms_set_av_error(context, "cannot open the decoder", result);
        return false;
    }

    snprintf(context->decoder_name, sizeof context->decoder_name, "%s%s",
        codec->name,
        context->decoder->hw_device_ctx != NULL ? " (vaapi)" : " (software)");

    const AVStream *stream = context->format_context->streams[context->stream_index];
    AVRational frame_rate = stream->avg_frame_rate;
    if (frame_rate.num <= 0 || frame_rate.den <= 0)
        frame_rate = stream->r_frame_rate;
    context->fallback_frame_interval_seconds =
        (frame_rate.num > 0 && frame_rate.den > 0)
            ? (double) frame_rate.den / (double) frame_rate.num
            : 1.0 / 60.0;

    context->packet = av_packet_alloc();
    context->frame = av_frame_alloc();
    if (context->packet == NULL || context->frame == NULL) {
        pms_set_error(context, "cannot allocate FFmpeg frame buffers");
        return false;
    }
    return true;
}

void pms_abi_sizes(uint32_t *config, uint32_t *surface_info,
                   uint32_t *frame_timing) {
    if (config != NULL)
        *config = (uint32_t) sizeof(struct pms_config);
    if (surface_info != NULL)
        *surface_info = (uint32_t) sizeof(struct pms_surface_info);
    if (frame_timing != NULL)
        *frame_timing = (uint32_t) sizeof(struct pms_frame_timing);
}

pms_context *pms_create(const struct pms_config *config) {
    if (config == NULL || config->width <= 0 || config->height <= 0)
        return NULL;

    pms_context *context = calloc(1, sizeof *context);
    if (context == NULL)
        return NULL;

    context->verbose = config->verbose;
    context->loop = config->loop;
    context->stream_index = -1;
    context->first_frame_seconds = -1.0;
    context->fallback_frame_interval_seconds = 1.0 / 60.0;
    context->render_completed_handle.fd = -1;
    context->available_handle.fd = -1;
    // Avalonia's importer issues no acquire barrier, so no queue family
    // ownership transfer is requested. Measured to be equivalent to
    // VK_QUEUE_FAMILY_EXTERNAL on this hardware.
    context->queue_family = VK_QUEUE_FAMILY_IGNORED;

    struct pl_log_params log_params = pl_log_default_params;
    log_params.log_cb = pms_log_callback;
    log_params.log_level = config->verbose ? PL_LOG_DEBUG : PL_LOG_WARN;
    context->log = pl_log_create(PL_API_VER, &log_params);

    struct pl_vulkan_params vulkan_params = pl_vulkan_default_params;
    if (config->has_device_uuid)
        memcpy(vulkan_params.device_uuid, config->device_uuid, 16);
    context->vulkan = pl_vulkan_create(context->log, &vulkan_params);
    if (context->vulkan == NULL) {
        pms_set_error(context, "pl_vulkan_create failed");
        return context;
    }

    pl_gpu gpu = context->vulkan->gpu;
    if ((gpu->export_caps.tex & PL_HANDLE_FD) == 0) {
        pms_set_error(context,
            "the selected device cannot export textures as opaque FDs");
        return context;
    }
    if ((gpu->export_caps.sync & PL_HANDLE_FD) == 0) {
        pms_set_error(context,
            "the selected device cannot export semaphores as opaque FDs");
        return context;
    }

    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties) context->vulkan->get_proc_addr(
            context->vulkan->instance, "vkGetPhysicalDeviceProperties");
    if (get_properties != NULL) {
        VkPhysicalDeviceProperties properties;
        get_properties(context->vulkan->phys_device, &properties);
        snprintf(context->device_name, sizeof context->device_name, "%s",
            properties.deviceName);
    }
    pms_resolve_render_node(context);

    // Avalonia's importer only understands R8G8B8A8_UNORM and B8G8R8A8_UNORM,
    // and creates the image with COLOR_ATTACHMENT | TRANSFER_DST | TRANSFER_SRC
    // | SAMPLED usage and OPTIMAL tiling. Matching that is what makes the
    // memory requirements line up on both sides.
    pl_fmt format = pl_find_named_fmt(gpu, "rgba8");
    if (format == NULL) {
        pms_set_error(context, "the rgba8 format is unavailable");
        return context;
    }

    struct pl_tex_params texture_params = {0};
    texture_params.w = config->width;
    texture_params.h = config->height;
    texture_params.format = format;
    texture_params.renderable = true;
    texture_params.sampleable = true;
    texture_params.blit_src = true;
    texture_params.blit_dst = true;
    texture_params.export_handle = PL_HANDLE_FD;
    context->texture = pl_tex_create(gpu, &texture_params);
    if (context->texture == NULL) {
        pms_set_error(context, "pl_tex_create with an exportable handle failed");
        return context;
    }

    struct pl_vulkan_sem_params semaphore_params = {0};
    semaphore_params.type = VK_SEMAPHORE_TYPE_BINARY;
    semaphore_params.export_handle = PL_HANDLE_FD;

    semaphore_params.out_handle = &context->render_completed_handle;
    context->render_completed = pl_vulkan_sem_create(gpu, &semaphore_params);
    if (context->render_completed == VK_NULL_HANDLE) {
        pms_set_error(context, "cannot export the render-completed semaphore");
        return context;
    }

    semaphore_params.out_handle = &context->available_handle;
    context->available = pl_vulkan_sem_create(gpu, &semaphore_params);
    if (context->available == VK_NULL_HANDLE) {
        pms_set_error(context, "cannot export the available semaphore");
        return context;
    }

    context->renderer = pl_renderer_create(context->log, gpu);
    if (context->renderer == NULL) {
        pms_set_error(context, "pl_renderer_create failed");
        return context;
    }

    if (config->input_path != NULL && config->input_path[0] != '\0' &&
        !pms_open_input(context, config))
        return context;

    return context;
}

int32_t pms_describe(pms_context *context, struct pms_surface_info *out_info) {
    if (context == NULL || out_info == NULL)
        return -1;
    if (context->texture == NULL || context->available == VK_NULL_HANDLE) {
        if (context->error[0] == '\0')
            pms_set_error(context, "the context was not fully initialised");
        return -1;
    }
    if (context->described) {
        pms_set_error(context, "pms_describe was already called");
        return -1;
    }

    memset(out_info, 0, sizeof *out_info);

    // libplacebo keeps ownership of the texture's own descriptor, so the copy
    // handed to the caller has to be a dup. The semaphore descriptors were
    // already exported to us and are passed on directly.
    const int image_fd = dup(context->texture->shared_mem.handle.fd);
    if (image_fd < 0) {
        pms_set_error(context, "cannot duplicate the exported image handle");
        return -1;
    }

    out_info->image_fd = image_fd;
    out_info->render_completed_semaphore_fd = context->render_completed_handle.fd;
    out_info->available_semaphore_fd = context->available_handle.fd;
    context->render_completed_handle.fd = -1;
    context->available_handle.fd = -1;

    out_info->memory_size = context->texture->shared_mem.size;
    out_info->memory_offset = context->texture->shared_mem.offset;
    out_info->width = context->texture->params.w;
    out_info->height = context->texture->params.h;
    // The stream geometry is known as soon as the codec is open; zero_copy is
    // not, it stays 0 until the first frame has actually been mapped.
    if (context->decoder != NULL) {
        out_info->video_width = context->video_width
            ? context->video_width : context->decoder->width;
        out_info->video_height = context->video_height
            ? context->video_height : context->decoder->height;
    }
    out_info->zero_copy = context->zero_copy;

    snprintf(out_info->device_name, sizeof out_info->device_name, "%s",
        context->device_name);
    snprintf(out_info->render_node, sizeof out_info->render_node, "%s",
        context->render_node);
    snprintf(out_info->decoder_name, sizeof out_info->decoder_name, "%s",
        context->decoder_name);

    context->described = 1;
    return 0;
}

// Pulls the next decoded frame into context->frame. Returns 1 on success, 0 at
// end of stream, and -1 on a hard decoder error.
static int pms_decode_next_frame(pms_context *context) {
    for (;;) {
        const int received = avcodec_receive_frame(context->decoder,
            context->frame);
        if (received == 0)
            return 1;
        if (received != AVERROR(EAGAIN) && received != AVERROR_EOF) {
            pms_set_av_error(context, "the decoder failed", received);
            return -1;
        }
        if (received == AVERROR_EOF)
            return 0;

        const int read = av_read_frame(context->format_context, context->packet);
        if (read < 0) {
            if (!context->loop) {
                avcodec_send_packet(context->decoder, NULL);
                const int drained = avcodec_receive_frame(context->decoder,
                    context->frame);
                return drained == 0 ? 1 : 0;
            }
            // Looping keeps the demo running without a growing decoder queue:
            // flush, seek back and start over.
            avcodec_flush_buffers(context->decoder);
            if (av_seek_frame(context->format_context, context->stream_index, 0,
                    AVSEEK_FLAG_BACKWARD) < 0)
                return 0;
            context->first_frame_seconds = -1.0;
            context->clock_origin_ms = pms_now_ms();
            continue;
        }

        if (context->packet->stream_index == context->stream_index) {
            const int sent = avcodec_send_packet(context->decoder, context->packet);
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                av_packet_unref(context->packet);
                pms_set_av_error(context, "the decoder rejected a packet", sent);
                return -1;
            }
        }
        av_packet_unref(context->packet);
    }
}

// Returns the frame's presentation time in seconds relative to the first frame,
// falling back to a fixed cadence for streams without timestamps.
static double pms_frame_offset_seconds(pms_context *context) {
    const AVStream *stream =
        context->format_context->streams[context->stream_index];
    const int64_t timestamp = context->frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE)
        return (double) context->presented_frames *
            context->fallback_frame_interval_seconds;

    const double seconds = (double) timestamp * av_q2d(stream->time_base);
    if (context->first_frame_seconds < 0.0)
        context->first_frame_seconds = seconds;
    return seconds - context->first_frame_seconds;
}

int32_t pms_render_frame(pms_context *context,
                         struct pms_frame_timing *out_timing) {
    if (context == NULL || context->texture == NULL ||
        context->renderer == NULL)
        return PMS_FRAME_ERROR;

    pl_gpu gpu = context->vulkan->gpu;
    double decode_ms = 0.0;
    double map_ms = 0.0;

    if (context->format_context != NULL) {
        if (context->stream_ended)
            return PMS_FRAME_END_OF_STREAM;

        if (!context->has_pending_frame) {
            const double decode_start = pms_now_ms();
            const int decoded = pms_decode_next_frame(context);
            decode_ms = pms_now_ms() - decode_start;
            if (decoded < 0)
                return PMS_FRAME_ERROR;
            if (decoded == 0) {
                context->stream_ended = 1;
                if (out_timing != NULL) {
                    memset(out_timing, 0, sizeof *out_timing);
                    out_timing->frames_rendered = context->frames_rendered;
                    out_timing->map_failures = context->map_failures;
                    out_timing->end_of_stream = 1;
                }
                return PMS_FRAME_END_OF_STREAM;
            }
            context->has_pending_frame = 1;
            if (context->video_width == 0) {
                context->video_width = context->frame->width;
                context->video_height = context->frame->height;
                context->zero_copy =
                    context->frame->format == AV_PIX_FMT_VAAPI ? 1 : 0;
            }
        }

        if (context->clock_origin_ms == 0.0)
            context->clock_origin_ms = pms_now_ms();

        const double due_ms = context->clock_origin_ms +
            pms_frame_offset_seconds(context) * 1000.0;
        if (pms_now_ms() < due_ms)
            return PMS_FRAME_NOT_READY;
    }

    double release_ms = 0.0;
    if (context->held_by_consumer) {
        const double release_start = pms_now_ms();
        struct pl_vulkan_release_params release_params = {0};
        release_params.tex = context->texture;
        release_params.layout = PMS_SHARED_LAYOUT;
        release_params.qf = context->queue_family;
        release_params.semaphore.sem = context->available;
        pl_vulkan_release_ex(gpu, &release_params);
        context->held_by_consumer = 0;
        release_ms = pms_now_ms() - release_start;
    }

    const double render_start = pms_now_ms();
    if (context->format_context == NULL) {
        // No input: animate a gradient so the handshake can still be exercised
        // and visually confirmed without media on disk.
        const double phase = (double) context->frames_rendered / 90.0;
        const float color[4] = {
            (float) (0.5 + 0.5 * __builtin_sin(phase)),
            (float) (0.5 + 0.5 * __builtin_sin(phase * 0.7 + 2.0)),
            (float) (0.5 + 0.5 * __builtin_sin(phase * 1.3 + 4.0)),
            1.0f,
        };
        pl_tex_clear(gpu, context->texture, color);
    } else {
        struct pl_frame image = {0};
        struct pl_avframe_params map_params = {0};
        map_params.frame = context->frame;
        map_params.tex = context->plane_textures;
        map_params.map_dovi = true;

        const double map_start = pms_now_ms();
        const bool mapped = pl_map_avframe_ex(gpu, &image, &map_params);
        map_ms = pms_now_ms() - map_start;
        if (!mapped) {
            context->map_failures++;
            av_frame_unref(context->frame);
            context->has_pending_frame = 0;
            pms_set_error(context,
                "pl_map_avframe_ex failed; the decoder's DRM format modifier is "
                "not importable by the render device");
            return PMS_FRAME_ERROR;
        }
        context->zero_copy = context->frame->format == AV_PIX_FMT_VAAPI ? 1 : 0;

        struct pl_frame target = {0};
        // flipped: Avalonia's importer samples the shared image with the
        // opposite Y convention than libplacebo renders in, which mirrored the
        // first real-video screenshot. Declaring the target flipped makes
        // libplacebo compensate; the gradient never showed this.
        pl_frame_from_swapchain(&target, &(struct pl_swapchain_frame) {
            .fbo = context->texture,
            .flipped = true,
            .color_repr = pl_color_repr_rgb,
            .color_space = pl_color_space_srgb,
        });

        // Letterbox rather than stretch: a phone screen is far taller than any
        // sane window, and the Windows preview keeps aspect ratio too.
        pl_rect2df_aspect_copy(&target.crop, &image.crop, 0.0f);

        // Only the letterboxed subrectangle is written, so the surround has to
        // be cleared or it would keep whatever the previous frame left behind.
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        pl_tex_clear(gpu, context->texture, black);

        const bool rendered = pl_render_image(context->renderer, &image, &target,
            &pl_render_default_params);
        pl_unmap_avframe(gpu, &image);
        av_frame_unref(context->frame);
        context->has_pending_frame = 0;
        if (!rendered) {
            pms_set_error(context, "pl_render_image failed");
            return PMS_FRAME_ERROR;
        }
        context->presented_frames++;
    }
    const double render_ms = pms_now_ms() - render_start;

    const double hold_start = pms_now_ms();
    struct pl_vulkan_hold_params hold_params = {0};
    hold_params.tex = context->texture;
    hold_params.layout = PMS_SHARED_LAYOUT;
    hold_params.qf = context->queue_family;
    hold_params.semaphore.sem = context->render_completed;
    if (!pl_vulkan_hold_ex(gpu, &hold_params)) {
        pms_set_error(context, "pl_vulkan_hold_ex failed");
        return PMS_FRAME_ERROR;
    }
    context->held_by_consumer = 1;
    const double hold_ms = pms_now_ms() - hold_start;

    context->frames_rendered++;
    if (out_timing != NULL) {
        out_timing->release_ms = release_ms;
        out_timing->decode_ms = decode_ms;
        out_timing->map_ms = map_ms;
        out_timing->render_ms = render_ms;
        out_timing->hold_ms = hold_ms;
        out_timing->frames_rendered = context->frames_rendered;
        out_timing->map_failures = context->map_failures;
        out_timing->end_of_stream = 0;
    }
    return PMS_FRAME_PRESENTED;
}

void pms_finish(pms_context *context) {
    if (context != NULL && context->vulkan != NULL)
        pl_gpu_finish(context->vulkan->gpu);
}

const char *pms_last_error(pms_context *context) {
    if (context == NULL)
        return "no context";
    return context->error;
}

void pms_destroy(pms_context *context) {
    if (context == NULL)
        return;

    av_frame_free(&context->frame);
    av_packet_free(&context->packet);
    avcodec_free_context(&context->decoder);
    av_buffer_unref(&context->hardware_device);
    if (context->format_context != NULL)
        avformat_close_input(&context->format_context);

    if (context->vulkan != NULL) {
        pl_gpu gpu = context->vulkan->gpu;
        // The consumer may still be waiting on a signalled semaphore, so drain
        // the device before any of the shared objects go away.
        pl_gpu_finish(gpu);
        if (context->held_by_consumer) {
            struct pl_vulkan_release_params release_params = {0};
            release_params.tex = context->texture;
            release_params.layout = PMS_SHARED_LAYOUT;
            release_params.qf = context->queue_family;
            pl_vulkan_release_ex(gpu, &release_params);
            context->held_by_consumer = 0;
        }
        pl_renderer_destroy(&context->renderer);
        for (int index = 0; index < 4; index++)
            pl_tex_destroy(gpu, &context->plane_textures[index]);
        if (context->available != VK_NULL_HANDLE)
            pl_vulkan_sem_destroy(gpu, &context->available);
        if (context->render_completed != VK_NULL_HANDLE)
            pl_vulkan_sem_destroy(gpu, &context->render_completed);
        pl_tex_destroy(gpu, &context->texture);
    }

    pms_close_handle(&context->available_handle);
    pms_close_handle(&context->render_completed_handle);
    pl_vulkan_destroy(&context->vulkan);
    pl_log_destroy(&context->log);
    free(context);
}
