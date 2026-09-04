# SPDX-License-Identifier: GPL-3.0-only
#
# Linux port spikes. These are read-only diagnostic probes used to answer the
# open questions in docs/LINUX_PORT.md before any Linux backend is written.
# They are not part of the shipped application and are excluded from install().

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    return()
endif()

# libplacebo's FFmpeg glue must be compiled once as C; see PlaceboAvGlue.c.
enable_language(C)

find_package(PkgConfig REQUIRED)

pkg_check_modules(IPHONEMIRROR_LIBUSB IMPORTED_TARGET libusb-1.0)
if(IPHONEMIRROR_LIBUSB_FOUND)
    add_executable(iPhoneMirror.Linux.AppleUsbEnumerationProbe
        AppleUsbEnumerationProbe.cpp)
    target_link_libraries(iPhoneMirror.Linux.AppleUsbEnumerationProbe PRIVATE
        iPhoneMirror.CompileOptions
        PkgConfig::IPHONEMIRROR_LIBUSB)
else()
    message(STATUS
        "linux-spikes: libusb-1.0 not found; skipping AppleUsbEnumerationProbe")
endif()

# Reuses this project's own plist implementation so the probe validates the
# real parser against usbmuxd rather than a throwaway copy.
add_executable(iPhoneMirror.Linux.UsbMuxUnixSocketProbe
    UsbMuxUnixSocketProbe.cpp
    ${PROJECT_SOURCE_DIR}/src/Core/src/Protocol/Plist.cpp)
target_include_directories(iPhoneMirror.Linux.UsbMuxUnixSocketProbe PRIVATE
    ${PROJECT_SOURCE_DIR}/src/Core/src)
target_link_libraries(iPhoneMirror.Linux.UsbMuxUnixSocketProbe PRIVATE
    iPhoneMirror.CompileOptions)

pkg_check_modules(IPHONEMIRROR_RENDER IMPORTED_TARGET
    libplacebo>=6 libavcodec libavformat libavutil)
if(IPHONEMIRROR_RENDER_FOUND)
    add_executable(iPhoneMirror.Linux.DecodeRenderLatencyProbe
        DecodeRenderLatencyProbe.cpp
        PlaceboAvGlue.c)
    target_link_libraries(iPhoneMirror.Linux.DecodeRenderLatencyProbe PRIVATE
        iPhoneMirror.CompileOptions
        PkgConfig::IPHONEMIRROR_RENDER)
else()
    message(STATUS
        "linux-spikes: libplacebo or FFmpeg not found; "
        "skipping DecodeRenderLatencyProbe")
endif()

# Native producer half of spikes S3 and S4. The managed consumer lives in
# AvaloniaSurfaceProbe/ and loads this shared object by name, which is why it is
# built as a library rather than linked into an executable. Vulkan is needed
# directly for VK_EXT_physical_device_drm, which maps the physical device the
# compositor selected onto the DRM render node the decoder must use.
pkg_check_modules(IPHONEMIRROR_PLACEBO IMPORTED_TARGET
    libplacebo>=6 vulkan libavcodec libavformat libavutil)
if(IPHONEMIRROR_PLACEBO_FOUND)
    add_library(iPhoneMirror.Linux.PlaceboSurfaceShim SHARED
        PlaceboSurfaceShim.c)
    set_target_properties(iPhoneMirror.Linux.PlaceboSurfaceShim PROPERTIES
        C_VISIBILITY_PRESET hidden
        OUTPUT_NAME iPhoneMirror.Linux.PlaceboSurfaceShim)
    target_link_libraries(iPhoneMirror.Linux.PlaceboSurfaceShim PRIVATE
        iPhoneMirror.CompileOptions
        PkgConfig::IPHONEMIRROR_PLACEBO)
else()
    message(STATUS
        "linux-spikes: libplacebo or Vulkan not found; "
        "skipping PlaceboSurfaceShim")
endif()
