// SPDX-License-Identifier: GPL-3.0-only
//
// Linux preview renderer seam: turns a DecodedFrame into display-ready RGBA on
// the GPU with libplacebo, which is what replaces the Windows D3D11 preview.
//
// This first step deliberately stops at "rendered pixels the host can read
// back". Exporting the image as a dmabuf and pairing it with Vulkan semaphores
// for Avalonia is the next step and belongs with the window that imports it;
// keeping them apart means the colour pipeline can be verified on its own,
// against this project's existing CPU colour maths, with no window and no
// device involved.

#pragma once

#include "Media/VideoFormats.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace iPhoneMirror::media {

// Everything an external Vulkan importer needs to bind the rendered image. The
// file descriptors stay owned by the renderer and are valid for its lifetime;
// an importer that needs its own lifetime must dup() them.
struct ExportedSurface {
    bool valid{};
    int memory_fd{-1};
    // Signalled by the renderer when a frame is finished; the importer waits on
    // it before sampling.
    int render_completed_fd{-1};
    // Signalled by the importer when it is done; the renderer waits on it before
    // touching the image again.
    int available_fd{-1};
    std::uint32_t width{};
    std::uint32_t height{};
    // Vulkan format enumerator, passed straight through so the importer does not
    // have to re-derive it from the layout.
    std::uint32_t vk_format{};
    std::uint64_t allocation_size{};
    std::uint64_t allocation_offset{};
};

class ILinuxPreviewRenderer {
public:
    virtual ~ILinuxPreviewRenderer() = default;

    // Renders one decoded frame into the internal target, letterboxed to
    // preserve aspect ratio. Returns false and sets last_error() on failure.
    [[nodiscard]] virtual bool present(const DecodedFrame& frame) = 0;
    // Downloads the target as tightly packed 8-bit RGBA. The span must hold
    // target_width() * target_height() * 4 bytes.
    [[nodiscard]] virtual bool read_back_rgba(std::span<std::uint8_t> destination) = 0;
    // Empty (valid == false) when the platform cannot export the image, which is
    // reported rather than treated as fatal: readback still works.
    [[nodiscard]] virtual ExportedSurface exported_surface() const noexcept = 0;

    [[nodiscard]] virtual std::uint32_t target_width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t target_height() const noexcept = 0;
    [[nodiscard]] virtual std::string_view device_name() const noexcept = 0;
    [[nodiscard]] virtual std::string_view last_error() const noexcept = 0;
};

// Throws std::runtime_error when Vulkan or libplacebo cannot be initialized,
// which on Linux is an ordinary outcome on a machine with no usable GPU. Never
// returns null.
[[nodiscard]] std::unique_ptr<ILinuxPreviewRenderer> make_placebo_preview_renderer(
    std::uint32_t target_width, std::uint32_t target_height);

} // namespace iPhoneMirror::media
