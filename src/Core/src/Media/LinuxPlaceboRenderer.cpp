// SPDX-License-Identifier: GPL-3.0-only
//
// libplacebo implementation of the Linux preview renderer.
//
// The frame arrives as the tightly packed semi-planar buffer the decoder seam
// promises, so both planes are uploaded as separate textures and described to
// libplacebo through pl_frame's plane/component model. Handing libplacebo the
// colour description from the frame rather than letting it guess is the whole
// point of carrying VideoColorDescription this far: the same metadata drives the
// GPU path here and the CPU path in VideoFrameCopy.cpp, so the two can be
// compared against each other.

#include "Media/LinuxPlaceboRenderer.h"

extern "C" {
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
}

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace iPhoneMirror::media {

namespace {

[[nodiscard]] pl_color_primaries map_primaries(
    coremedia::ColorPrimaries value) noexcept {
    switch (value) {
    case coremedia::ColorPrimaries::Bt709: return PL_COLOR_PRIM_BT_709;
    case coremedia::ColorPrimaries::Bt2020: return PL_COLOR_PRIM_BT_2020;
    case coremedia::ColorPrimaries::DisplayP3: return PL_COLOR_PRIM_DISPLAY_P3;
    case coremedia::ColorPrimaries::Unspecified: break;
    }
    return PL_COLOR_PRIM_UNKNOWN;
}

[[nodiscard]] pl_color_transfer map_transfer(
    coremedia::TransferFunction value) noexcept {
    switch (value) {
    case coremedia::TransferFunction::Bt709: return PL_COLOR_TRC_BT_1886;
    case coremedia::TransferFunction::Srgb: return PL_COLOR_TRC_SRGB;
    case coremedia::TransferFunction::Pq: return PL_COLOR_TRC_PQ;
    case coremedia::TransferFunction::Hlg: return PL_COLOR_TRC_HLG;
    case coremedia::TransferFunction::Unspecified: break;
    }
    return PL_COLOR_TRC_UNKNOWN;
}

[[nodiscard]] pl_color_system map_matrix(
    coremedia::MatrixCoefficients value) noexcept {
    switch (value) {
    case coremedia::MatrixCoefficients::Bt601: return PL_COLOR_SYSTEM_BT_601;
    case coremedia::MatrixCoefficients::Bt709: return PL_COLOR_SYSTEM_BT_709;
    case coremedia::MatrixCoefficients::Bt2020: return PL_COLOR_SYSTEM_BT_2020_NC;
    case coremedia::MatrixCoefficients::Unspecified: break;
    }
    return PL_COLOR_SYSTEM_UNKNOWN;
}

class PlaceboPreviewRenderer final : public ILinuxPreviewRenderer {
public:
    PlaceboPreviewRenderer(std::uint32_t target_width,
        std::uint32_t target_height)
        : width_(target_width), height_(target_height) {
        if (width_ == 0 || height_ == 0)
            throw std::invalid_argument("the preview target has no area");

        struct pl_log_params log_params = pl_log_default_params;
        log_params.log_level = PL_LOG_WARN;
        log_ = pl_log_create(PL_API_VER, &log_params);
        if (log_ == nullptr) throw std::runtime_error("pl_log_create failed");

        struct pl_vulkan_params vulkan_params = pl_vulkan_default_params;
        vulkan_ = pl_vulkan_create(log_, &vulkan_params);
        if (vulkan_ == nullptr) {
            destroy();
            throw std::runtime_error("pl_vulkan_create failed: no usable Vulkan device");
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(vulkan_->phys_device, &properties);
        device_name_ = properties.deviceName;

        struct pl_tex_params target_params = {};
        target_params.w = static_cast<int>(width_);
        target_params.h = static_cast<int>(height_);
        target_params.format = pl_find_fmt(vulkan_->gpu, PL_FMT_UNORM, 4, 8, 8,
            static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
        if (target_params.format == nullptr) {
            destroy();
            throw std::runtime_error("no renderable host-readable RGBA8 format");
        }
        target_params.renderable = true;
        target_params.host_readable = true;
        target_ = pl_tex_create(vulkan_->gpu, &target_params);
        if (target_ == nullptr) {
            destroy();
            throw std::runtime_error("pl_tex_create failed for the preview target");
        }

        renderer_ = pl_renderer_create(log_, vulkan_->gpu);
        if (renderer_ == nullptr) {
            destroy();
            throw std::runtime_error("pl_renderer_create failed");
        }
    }

    ~PlaceboPreviewRenderer() override { destroy(); }

    PlaceboPreviewRenderer(const PlaceboPreviewRenderer&) = delete;
    PlaceboPreviewRenderer& operator=(const PlaceboPreviewRenderer&) = delete;

    [[nodiscard]] bool present(const DecodedFrame& frame) override {
        last_error_.clear();
        if (frame.width == 0 || frame.height == 0 || frame.nv12.empty())
            return fail("the decoded frame is empty");
        const bool ten_bit = frame.pixel_format == PixelFormat::P010;
        const int component_bytes = ten_bit ? 2 : 1;
        const auto luma_stride = static_cast<std::size_t>(frame.stride);
        const auto luma_bytes = luma_stride * frame.height;
        const auto chroma_bytes = luma_stride * (frame.height / 2U);
        if (frame.nv12.size() < luma_bytes + chroma_bytes)
            return fail("the decoded frame is shorter than its own geometry");

        // Plane 0 is single-component luma, plane 1 is the interleaved chroma
        // pair. Both are the same stride because the buffer is tightly packed.
        if (!upload_plane(0, frame.nv12.data(), luma_stride, frame.width,
                frame.height, 1, component_bytes)) {
            return false;
        }
        if (!upload_plane(1, frame.nv12.data() + luma_bytes, luma_stride,
                frame.width / 2U, frame.height / 2U, 2, component_bytes)) {
            return false;
        }

        struct pl_frame image = {};
        image.num_planes = 2;
        image.planes[0].texture = planes_[0];
        image.planes[0].components = 1;
        image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
        image.planes[1].texture = planes_[1];
        image.planes[1].components = 2;
        image.planes[1].component_mapping[0] = PL_CHANNEL_U;
        image.planes[1].component_mapping[1] = PL_CHANNEL_V;
        image.repr.sys = map_matrix(frame.color.matrix);
        image.repr.levels = frame.color.range == coremedia::ColorRange::Full
            ? PL_COLOR_LEVELS_FULL
            : PL_COLOR_LEVELS_LIMITED;
        // P010 carries 10 significant bits in the high end of each 16-bit word;
        // saying so is what stops libplacebo treating it as a 16-bit signal.
        image.repr.bits.sample_depth = ten_bit ? 16 : 8;
        image.repr.bits.color_depth = ten_bit ? 10 : 8;
        image.repr.bits.bit_shift = ten_bit ? 6 : 0;
        image.color.primaries = map_primaries(frame.color.primaries);
        image.color.transfer = map_transfer(frame.color.transfer);
        image.crop = pl_rect2df{0.0F, 0.0F, static_cast<float>(frame.width),
            static_cast<float>(frame.height)};

        struct pl_frame target = {};
        target.num_planes = 1;
        target.planes[0].texture = target_;
        target.planes[0].components = 4;
        target.planes[0].component_mapping[0] = PL_CHANNEL_R;
        target.planes[0].component_mapping[1] = PL_CHANNEL_G;
        target.planes[0].component_mapping[2] = PL_CHANNEL_B;
        target.planes[0].component_mapping[3] = PL_CHANNEL_A;
        target.repr.sys = PL_COLOR_SYSTEM_RGB;
        target.repr.levels = PL_COLOR_LEVELS_FULL;
        target.repr.bits.sample_depth = 8;
        target.repr.bits.color_depth = 8;
        target.color = pl_color_space_srgb;
        target.crop = pl_rect2df{0.0F, 0.0F, static_cast<float>(width_),
            static_cast<float>(height_)};
        // Letterbox rather than stretch: the phone's aspect ratio is the point.
        pl_rect2df_aspect_copy(&target.crop, &image.crop, 0.0F);

        if (!pl_render_image(renderer_, &image, &target,
                &pl_render_default_params)) {
            return fail("pl_render_image failed");
        }
        return true;
    }

    [[nodiscard]] bool read_back_rgba(
        std::span<std::uint8_t> destination) override {
        last_error_.clear();
        const auto required = static_cast<std::size_t>(width_) * height_ * 4U;
        if (destination.size() < required)
            return fail("the readback buffer is smaller than the target");
        struct pl_tex_transfer_params transfer = {};
        transfer.tex = target_;
        transfer.ptr = destination.data();
        if (!pl_tex_download(vulkan_->gpu, &transfer))
            return fail("pl_tex_download failed");
        return true;
    }

    [[nodiscard]] std::uint32_t target_width() const noexcept override {
        return width_;
    }
    [[nodiscard]] std::uint32_t target_height() const noexcept override {
        return height_;
    }
    [[nodiscard]] std::string_view device_name() const noexcept override {
        return device_name_;
    }
    [[nodiscard]] std::string_view last_error() const noexcept override {
        return last_error_;
    }

private:
    [[nodiscard]] bool fail(std::string message) {
        last_error_ = std::move(message);
        return false;
    }

    // Recreates the plane texture when the geometry or depth changed, then
    // uploads. Keeping the textures between frames is what makes a steady stream
    // cheap; the capture geometry only changes on rotation.
    [[nodiscard]] bool upload_plane(std::size_t index, const std::uint8_t* data,
        std::size_t stride, std::uint32_t width, std::uint32_t height,
        int components, int component_bytes) {
        if (width == 0 || height == 0) return fail("a frame plane has no area");
        pl_fmt format = pl_find_fmt(vulkan_->gpu, PL_FMT_UNORM, components,
            component_bytes * 8, component_bytes * 8,
            static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
        if (format == nullptr) {
            return fail(std::format("no sampleable {}x{}-bit plane format",
                components, component_bytes * 8));
        }
        struct pl_tex_params params = {};
        params.w = static_cast<int>(width);
        params.h = static_cast<int>(height);
        params.format = format;
        params.sampleable = true;
        params.host_writable = true;
        if (!pl_tex_recreate(vulkan_->gpu, &planes_[index], &params))
            return fail("pl_tex_recreate failed for a frame plane");

        struct pl_tex_transfer_params transfer = {};
        transfer.tex = planes_[index];
        transfer.ptr = const_cast<std::uint8_t*>(data);
        transfer.row_pitch = stride;
        if (!pl_tex_upload(vulkan_->gpu, &transfer))
            return fail("pl_tex_upload failed for a frame plane");
        return true;
    }

    void destroy() noexcept {
        if (vulkan_ != nullptr) {
            for (auto& plane : planes_) pl_tex_destroy(vulkan_->gpu, &plane);
            pl_tex_destroy(vulkan_->gpu, &target_);
        }
        pl_renderer_destroy(&renderer_);
        pl_vulkan_destroy(&vulkan_);
        pl_log_destroy(&log_);
    }

    std::uint32_t width_{};
    std::uint32_t height_{};
    pl_log log_{};
    pl_vulkan vulkan_{};
    pl_renderer renderer_{};
    pl_tex target_{};
    pl_tex planes_[2]{};
    std::string device_name_;
    std::string last_error_;
};

} // namespace

std::unique_ptr<ILinuxPreviewRenderer> make_placebo_preview_renderer(
    std::uint32_t target_width, std::uint32_t target_height) {
    return std::make_unique<PlaceboPreviewRenderer>(target_width, target_height);
}

} // namespace iPhoneMirror::media
