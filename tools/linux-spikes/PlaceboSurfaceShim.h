// SPDX-License-Identifier: GPL-3.0-only
//
// Linux port spike S3/S4: native producer for the Avalonia preview surface.
//
// This is the Linux counterpart of the Windows preview path. Windows decodes
// with Media Foundation into a shared NV12 texture, converts it with a D3D11
// shader and presents it through DirectComposition. Here the chain is FFmpeg
// (VAAPI or software) -> libplacebo on Vulkan -> an image exported as an opaque
// POSIX file descriptor -> Avalonia's ICompositionGpuInterop.
//
// The decode device is not configurable by design. Avalonia only accepts an
// imported image that lives on the physical device its compositor selected, and
// libplacebo can only map a VAAPI surface whose DRM format modifier the render
// device accepts. Both constraints resolve to the same GPU, so the shim derives
// the DRM render node from the Vulkan physical device through
// VK_EXT_physical_device_drm rather than taking a node path from the caller.
//
// The ABI is flat and blittable so the managed side needs no marshalling
// helpers.

#ifndef IPHONEMIRROR_LINUX_PLACEBO_SURFACE_SHIM_H
#define IPHONEMIRROR_LINUX_PLACEBO_SURFACE_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMS_EXPORT __attribute__((visibility("default")))

// Return codes for pms_render_frame. NOT_READY is not an error: the next frame
// has been decoded but its presentation time has not arrived, so the caller must
// requeue without counting a presented frame.
#define PMS_FRAME_PRESENTED 0
#define PMS_FRAME_NOT_READY 1
#define PMS_FRAME_END_OF_STREAM 2
#define PMS_FRAME_ERROR (-1)

typedef struct pms_context pms_context;

// Layout must stay in sync with PlaceboSurface.Config on the managed side.
struct pms_config {
    // The Vulkan physical device the Avalonia compositor runs on, as reported
    // by ICompositionGpuInterop.DeviceUuid. Opaque FD memory cannot be shared
    // across physical devices, so the producer must be pinned to the exact
    // device the compositor selected rather than to libplacebo's own
    // preference for the discrete GPU.
    uint8_t device_uuid[16];
    int32_t has_device_uuid;

    // Size of the exported image. The decoded video is letterboxed into it, so
    // this is the surface resolution rather than the stream resolution.
    int32_t width;
    int32_t height;

    // Elementary stream or container to play. When NULL the shim renders an
    // animated gradient, which still exercises the whole export/import/present
    // handshake without needing media on disk.
    const char *input_path;

    // Skip VAAPI and decode on the CPU. Needed on hardware where the decoder's
    // DRM format modifier is not accepted by the render device.
    int32_t force_software_decode;

    // Restart the stream when it ends instead of holding the last frame.
    int32_t loop;

    int32_t verbose;
};

// Layout must stay in sync with PlaceboSurface.SurfaceInfo on the managed side.
struct pms_surface_info {
    // Ownership of all three descriptors transfers to the caller, which is what
    // Avalonia's ImportImage and ImportSemaphore expect.
    int32_t image_fd;
    int32_t render_completed_semaphore_fd;
    int32_t available_semaphore_fd;

    // Avalonia rejects the import unless this matches
    // vkGetImageMemoryRequirements().size exactly and the offset is zero.
    uint64_t memory_size;
    uint64_t memory_offset;

    int32_t width;
    int32_t height;

    // Decoded stream geometry, or zero when no input was given.
    int32_t video_width;
    int32_t video_height;

    // Nonzero once a hardware surface has been mapped without a host copy.
    int32_t zero_copy;

    // The physical device the shim selected, the DRM render node derived from
    // it, and the decoder actually in use, for cross-checking against the
    // compositor's device.
    char device_name[256];
    char render_node[64];
    char decoder_name[64];
};

// Layout must stay in sync with PlaceboSurface.FrameTiming on the managed side.
struct pms_frame_timing {
    double release_ms;
    double decode_ms;
    double map_ms;
    double render_ms;
    double hold_ms;
    int64_t frames_rendered;
    int64_t map_failures;
    // Nonzero when the stream ended and looping is disabled.
    int32_t end_of_stream;
};

// Reports the size of each struct above so the managed side can compare them
// against its own Marshal.SizeOf before the first call. A silent layout drift
// here is not a wrong pixel: pms_describe writes the whole of
// pms_surface_info into the caller's buffer, so a short managed struct
// corrupts whatever follows it.
PMS_EXPORT void pms_abi_sizes(uint32_t *config, uint32_t *surface_info,
                              uint32_t *frame_timing);

// Creates the Vulkan device, the exportable render target, the exportable
// semaphore pair and, when an input path is given, the decoder. Returns NULL
// only on allocation failure; every other error is reported through
// pms_last_error so the managed side can surface the reason.
PMS_EXPORT pms_context *pms_create(const struct pms_config *config);

// Fills `out_info` and transfers descriptor ownership to the caller. Must be
// called exactly once per context. Returns 0 on success.
PMS_EXPORT int32_t pms_describe(pms_context *context,
                                struct pms_surface_info *out_info);

// Waits for the consumer through the available semaphore, decodes and renders
// one frame, then signals the render-completed semaphore. Returns one of the
// PMS_FRAME_* codes; only PMS_FRAME_PRESENTED means a new image is ready for
// the consumer to import.
PMS_EXPORT int32_t pms_render_frame(pms_context *context,
                                    struct pms_frame_timing *out_timing);

// Blocks until every submitted command has completed. Used before teardown so
// the semaphore payloads are not destroyed while still pending.
PMS_EXPORT void pms_finish(pms_context *context);

PMS_EXPORT const char *pms_last_error(pms_context *context);
PMS_EXPORT void pms_destroy(pms_context *context);

#ifdef __cplusplus
}
#endif

#endif // IPHONEMIRROR_LINUX_PLACEBO_SURFACE_SHIM_H
