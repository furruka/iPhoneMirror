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

class ILinuxPreviewRenderer {
public:
    virtual ~ILinuxPreviewRenderer() = default;

    // Renders one decoded frame into the internal target, letterboxed to
    // preserve aspect ratio. Returns false and sets last_error() on failure.
    [[nodiscard]] virtual bool present(const DecodedFrame& frame) = 0;
    // Downloads the target as tightly packed 8-bit RGBA. The span must hold
    // target_width() * target_height() * 4 bytes.
    [[nodiscard]] virtual bool read_back_rgba(std::span<std::uint8_t> destination) = 0;

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
