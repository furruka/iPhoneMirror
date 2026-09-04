cmake_minimum_required(VERSION 3.25)

project(iPhoneMirror VERSION 1.8.1 LANGUAGES CXX)

if(NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR
        "iPhoneMirror targets Windows 10/11 and Linux; "
        "${CMAKE_SYSTEM_NAME} is not supported")
endif()

option(IPHONEMIRROR_BUILD_TESTS "Build native protocol tests" ON)
option(IPHONEMIRROR_BUILD_DANGEROUS_USB_TOOLS
    "Build tools that issue real Apple USB configuration and bulk requests" OFF)
option(IPHONEMIRROR_BUILD_LINUX_SPIKES
    "Build the read-only Linux port diagnostic probes" OFF)

if(IPHONEMIRROR_BUILD_TESTS)
    enable_testing()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Warning and encoding flags differ between MSVC and the GCC/Clang front ends.
# Targets consume this interface library instead of hard-coding /W4.
add_library(iPhoneMirror.CompileOptions INTERFACE)
if(MSVC)
    target_compile_options(iPhoneMirror.CompileOptions INTERFACE
        /W4 /permissive- /utf-8 /EHsc)
else()
    # -Wno-missing-field-initializers keeps designated initializers as quiet as
    # they already are under MSVC /W4, so the Linux build does not report
    # warnings that upstream never had to fix.
    target_compile_options(iPhoneMirror.CompileOptions INTERFACE
        -Wall -Wextra -Wno-missing-field-initializers
        -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

add_subdirectory(src/Core)
add_subdirectory(src/WirelessHost)
if(WIN32)
    # Media Foundation virtual camera. The Linux equivalent (v4l2loopback or a
    # PipeWire virtual node) is not implemented yet, and no part of this module
    # is platform neutral; see docs/LINUX_PORT.md.
    add_subdirectory(src/VirtualCamera)
endif()
if(IPHONEMIRROR_BUILD_LINUX_SPIKES)
    add_subdirectory(tools/linux-spikes)
endif()
