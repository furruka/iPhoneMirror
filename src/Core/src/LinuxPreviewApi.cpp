// SPDX-License-Identifier: GPL-3.0-only
//
// Implementation of the Linux preview C ABI. Thin on purpose: it owns one
// renderer and translates the C++ seam into the flat struct the managed side
// mirrors. All of the interesting work is in Media/LinuxPlaceboRenderer.cpp.

#include "iPhoneMirror/LinuxPreviewApi.h"

#include "Media/LinuxPlaceboRenderer.h"
#include "Logging.h"
#include "Text/Utf.h"

#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace {

std::mutex preview_mutex;
std::unique_ptr<iPhoneMirror::media::ILinuxPreviewRenderer> renderer;

// The shared C ABI keeps its own last-error string; this one is local so a
// preview failure cannot clobber a capture diagnostic the caller has not read.
std::string preview_error;

void set_error(std::string message) {
    preview_error = std::move(message);
    iPhoneMirror::logging::write(std::format("linux_preview {}", preview_error));
}

} // namespace

std::uint32_t IM_CALL im_linux_preview_abi_size() {
    return static_cast<std::uint32_t>(sizeof(iPhoneMirror::LinuxPreviewSurface));
}

std::int32_t IM_CALL im_linux_preview_open(std::uint32_t width,
    std::uint32_t height, const std::uint8_t* device_uuid) {
    std::scoped_lock lock(preview_mutex);
    try {
        renderer = iPhoneMirror::media::make_placebo_preview_renderer(width,
            height, device_uuid);
        preview_error.clear();
        return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
    } catch (const std::exception& error) {
        renderer.reset();
        set_error(error.what());
        return static_cast<std::int32_t>(
            iPhoneMirror::Result::CaptureBackendUnavailable);
    }
}

void IM_CALL im_linux_preview_close() {
    std::scoped_lock lock(preview_mutex);
    renderer.reset();
}

std::int32_t IM_CALL im_linux_preview_describe(
    iPhoneMirror::LinuxPreviewSurface* surface) {
    if (surface == nullptr)
        return static_cast<std::int32_t>(iPhoneMirror::Result::InvalidArgument);
    std::scoped_lock lock(preview_mutex);
    if (!renderer) {
        set_error("no preview is open");
        return static_cast<std::int32_t>(
            iPhoneMirror::Result::CaptureBackendUnavailable);
    }
    const auto exported = renderer->exported_surface();
    *surface = iPhoneMirror::LinuxPreviewSurface{
        .valid = exported.valid ? 1 : 0,
        .memory_fd = exported.memory_fd,
        .render_completed_fd = exported.render_completed_fd,
        .available_fd = exported.available_fd,
        .width = exported.width,
        .height = exported.height,
        .vk_format = exported.vk_format,
        .reserved = 0,
        .allocation_size = exported.allocation_size,
        .allocation_offset = exported.allocation_offset,
    };
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

std::int32_t IM_CALL im_linux_preview_present_nv12(const std::uint8_t* data,
    std::uint64_t size, std::uint32_t width, std::uint32_t height) {
    if (data == nullptr || width == 0 || height == 0)
        return static_cast<std::int32_t>(iPhoneMirror::Result::InvalidArgument);
    const auto required = static_cast<std::uint64_t>(width) * height * 3U / 2U;
    if (size < required) {
        set_error(std::format("{} bytes is short of the {} an {}x{} NV12 frame "
                              "needs", size, required, width, height));
        return static_cast<std::int32_t>(iPhoneMirror::Result::InvalidArgument);
    }
    std::scoped_lock lock(preview_mutex);
    if (!renderer) {
        set_error("no preview is open");
        return static_cast<std::int32_t>(
            iPhoneMirror::Result::CaptureBackendUnavailable);
    }

    iPhoneMirror::media::DecodedFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = static_cast<std::int32_t>(width);
    frame.pixel_format = iPhoneMirror::media::PixelFormat::Nv12;
    // The same defaults the decoder applies to an unspecified stream, so a
    // caller feeding raw bytes gets the colour treatment a decoded frame gets.
    frame.color.primaries = iPhoneMirror::coremedia::ColorPrimaries::Bt709;
    frame.color.transfer = iPhoneMirror::coremedia::TransferFunction::Bt709;
    frame.color.matrix = height >= 720
        ? iPhoneMirror::coremedia::MatrixCoefficients::Bt709
        : iPhoneMirror::coremedia::MatrixCoefficients::Bt601;
    frame.color.range = iPhoneMirror::coremedia::ColorRange::Limited;
    frame.nv12.assign(data, data + required);

    if (!renderer->present(frame)) {
        set_error(std::string(renderer->last_error()));
        return static_cast<std::int32_t>(
            iPhoneMirror::Result::CaptureBackendUnavailable);
    }
    preview_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}
