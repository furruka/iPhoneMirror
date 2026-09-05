// SPDX-License-Identifier: GPL-3.0-only
//
// Linux-only C ABI for the exported preview surface.
//
// A separate header rather than additions to CoreApi.h, for two reasons. The
// shared header's preview API is HWND-based (im_attach_preview_window takes a
// void* hwnd) and there is no honest Linux implementation of that shape: the
// agreed model hands the window an exported Vulkan image instead of a native
// window handle to draw into. And the port's rule is that Linux backends are new
// files, so the shared header keeps its ApiVersion and its surface untouched.
//
// The managed side P/Invokes these three functions and imports the descriptors
// through Avalonia's ICompositionGpuInterop.

#pragma once

#include "iPhoneMirror/CoreApi.h"

#include <cstdint>

namespace iPhoneMirror {

// Layout is fixed: the managed struct mirrors it field for field, and
// im_linux_preview_abi_size exists so a mismatch fails loudly at startup rather
// than corrupting memory the way the S3 spike's did.
struct LinuxPreviewSurface {
    std::int32_t valid;
    std::int32_t memory_fd;
    std::int32_t render_completed_fd;
    std::int32_t available_fd;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t vk_format;
    std::uint32_t reserved;
    std::uint64_t allocation_size;
    std::uint64_t allocation_offset;
};

} // namespace iPhoneMirror

// Size of LinuxPreviewSurface as this library sees it.
IM_API std::uint32_t IM_CALL im_linux_preview_abi_size();

// Creates the preview renderer at the given target size. device_uuid points at
// the importer's 16-byte Vulkan physical-device UUID, or is null to let
// libplacebo choose. Pass it: on a multi-GPU machine an image exported from one
// device cannot be imported by a compositor on another, and the failure surfaces
// only when the first frame is presented.
//
// Returns Ok, or CaptureBackendUnavailable with the reason recorded — a machine
// with no usable Vulkan device is an ordinary case, not a crash.
IM_API std::int32_t IM_CALL im_linux_preview_open(std::uint32_t width,
    std::uint32_t height, const std::uint8_t* device_uuid);

IM_API void IM_CALL im_linux_preview_close();

// Fills the descriptor the importer binds. Fails when no preview is open or the
// platform cannot export, and in the latter case `valid` is zero rather than the
// call reporting success.
IM_API std::int32_t IM_CALL im_linux_preview_describe(
    iPhoneMirror::LinuxPreviewSurface* surface);

// Renders one tightly packed NV12 frame. Present so the surface can be exercised
// before the capture session is wired to the renderer; the streaming path will
// drive the renderer directly rather than through this call.
IM_API std::int32_t IM_CALL im_linux_preview_present_nv12(
    const std::uint8_t* data, std::uint64_t size, std::uint32_t width,
    std::uint32_t height);
