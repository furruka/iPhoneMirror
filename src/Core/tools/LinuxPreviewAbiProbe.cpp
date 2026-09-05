// SPDX-License-Identifier: GPL-3.0-only
//
// Acceptance tool for the Linux preview C ABI. Goes through the exported
// im_linux_preview_* entry points rather than the C++ seam, because the ABI is
// what the Avalonia shell will call and an ABI can be wrong in ways the C++
// interface cannot.
//
// Needs no device. It feeds a raw NV12 frame, which is what LinuxDecodeProbe
// writes, so the input is already known good.

#include "iPhoneMirror/LinuxPreviewApi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: iPhoneMirror.Linux.PreviewAbiProbe "
                             "<input.nv12> <width> <height>\n");
        return 2;
    }
    const auto width = static_cast<std::uint32_t>(std::atoi(argv[2]));
    const auto height = static_cast<std::uint32_t>(std::atoi(argv[3]));
    if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0) {
        std::fprintf(stderr, "the geometry must be even and non-zero\n");
        return 2;
    }

    const auto reported = im_linux_preview_abi_size();
    const auto local = static_cast<std::uint32_t>(
        sizeof(iPhoneMirror::LinuxPreviewSurface));
    std::printf("abi size              : library=%u caller=%u %s\n", reported,
        local, reported == local ? "match" : "MISMATCH");
    if (reported != local) {
        std::fprintf(stderr, "the struct layouts disagree; refusing to call on\n");
        return 1;
    }

    const auto frame_bytes = static_cast<std::size_t>(width) * height * 3U / 2U;
    std::vector<std::uint8_t> frame(frame_bytes);
    std::FILE* file = std::fopen(argv[1], "rb");
    if (file == nullptr ||
        std::fread(frame.data(), 1, frame_bytes, file) != frame_bytes) {
        if (file != nullptr) std::fclose(file);
        std::fprintf(stderr, "cannot read %zu bytes from %s\n", frame_bytes,
            argv[1]);
        return 1;
    }
    std::fclose(file);

    if (const auto status = im_linux_preview_open(width, height, nullptr); status != 0) {
        std::fprintf(stderr, "im_linux_preview_open failed: %d\n", status);
        return 1;
    }

    iPhoneMirror::LinuxPreviewSurface surface{};
    if (const auto status = im_linux_preview_describe(&surface); status != 0) {
        std::fprintf(stderr, "im_linux_preview_describe failed: %d\n", status);
        im_linux_preview_close();
        return 1;
    }
    std::printf("surface               : valid=%d %ux%u vk_format=%u\n",
        surface.valid, surface.width, surface.height, surface.vk_format);
    std::printf("descriptors           : memory=%d done=%d free=%d\n",
        surface.memory_fd, surface.render_completed_fd, surface.available_fd);
    std::printf("allocation            : size=%llu offset=%llu\n",
        static_cast<unsigned long long>(surface.allocation_size),
        static_cast<unsigned long long>(surface.allocation_offset));

    const auto status = im_linux_preview_present_nv12(frame.data(),
        frame.size(), width, height);
    std::printf("present               : %s (%d)\n", status == 0 ? "ok" : "failed",
        status);
    im_linux_preview_close();

    if (status != 0) return 1;
    // A descriptor-less surface still renders, so it is reported rather than
    // failed: the importer is what needs the descriptors, and this tool is not it.
    if (surface.valid == 0) {
        std::printf("verdict               : PASS (no exportable surface here)\n");
        return 0;
    }
    std::printf("verdict               : PASS\n");
    return 0;
}
