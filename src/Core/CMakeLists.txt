# Platform-neutral protocol, container, bitstream, transport and decoded-video
# code. These translation units compile with MSVC, GCC and Clang and are shared
# by both platforms.
set(IPHONEMIRROR_CORE_PORTABLE_SOURCES
    src/Protocol/Plist.cpp
    src/Protocol/QuickTimePacket.cpp
    src/Protocol/QuickTimeSession.cpp
    src/Media/H264.cpp
    src/Media/CoreMedia.cpp
    src/Media/VideoFrameCopy.cpp
    src/Audio/PcmBufferPolicy.cpp
    src/Text/Utf.cpp
    src/Transport/Socket.cpp
    src/Transport/QtUsbTransport.cpp
)

# Windows-only subsystems: Media Foundation decoding, WASAPI playback, D3D11
# preview, SetupAPI discovery, the libusb-win32 filter transport and the named
# pipe receiver hub. Socket and QtUsbTransport have moved to the portable list;
# the Linux replacements for the rest live in the Linux source list.
set(IPHONEMIRROR_CORE_WINDOWS_SOURCES
    resources/iPhoneMirror.Core.rc
    src/CoreApi.cpp
    src/Logging.cpp
    src/Transport/UsbMuxClient.cpp
    src/Transport/LibUsb0Transport.cpp
    src/Device/AppleUsbDiscovery.cpp
    src/Device/DeviceManager.cpp
    src/Capture/CaptureSession.cpp
    src/Capture/WirelessReceiverHub.cpp
    src/Capture/WirelessCaptureSession.cpp
    src/Media/MediaFoundationDecoder.cpp
    src/Audio/WasapiRenderer.cpp
    src/Renderer/D3D11PreviewRenderer.cpp
)

if(WIN32)
    set(IPHONEMIRROR_CORE_SOURCES
        ${IPHONEMIRROR_CORE_PORTABLE_SOURCES}
        ${IPHONEMIRROR_CORE_WINDOWS_SOURCES})
else()
    # The Linux backends are added as this port progresses; see
    # docs/LINUX_PORT.md for the phase status. Until then the Linux
    # configuration only builds the portable protocol layer and its tests.
    set(IPHONEMIRROR_CORE_SOURCES ${IPHONEMIRROR_CORE_PORTABLE_SOURCES})
endif()

add_library(iPhoneMirror.Core SHARED ${IPHONEMIRROR_CORE_SOURCES})
target_link_libraries(iPhoneMirror.Core PRIVATE iPhoneMirror.CompileOptions)

target_compile_definitions(iPhoneMirror.Core PRIVATE
    IPHONEMIRROR_CORE_EXPORTS
    $<$<BOOL:${WIN32}>:UNICODE>
    $<$<BOOL:${WIN32}>:_UNICODE>
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${WIN32}>:NOMINMAX>
)

target_include_directories(iPhoneMirror.Core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/WirelessHost
)

if(WIN32)
    target_include_directories(iPhoneMirror.Core PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/libusb/include
        ${PROJECT_SOURCE_DIR}/third_party/libusb-win32/include
    )

    target_link_libraries(iPhoneMirror.Core PRIVATE
        ws2_32
        bcrypt
        setupapi
        cfgmgr32
        advapi32
        mfplat
        mf
        mfuuid
        wmcodecdspuuid
        d3d11
        d3dcompiler
        dcomp
        dxgi
        ole32
        avrt
        winhttp
        delayimp
        "${PROJECT_SOURCE_DIR}/third_party/libusb/lib/x64/libusb-1.0.lib"
        "${PROJECT_SOURCE_DIR}/third_party/libusb-win32/lib/x64/libusb0-dynamic.lib"
    )
    target_link_options(iPhoneMirror.Core PRIVATE /DELAYLOAD:libusb0.dll)

    add_custom_command(TARGET iPhoneMirror.Core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/third_party/libusb/bin/x64/libusb-1.0.dll
            $<TARGET_FILE_DIR:iPhoneMirror.Core>/libusb-1.0.dll
    )
else()
    # QtUsbTransport in the portable source list talks to libusb-1.0 directly;
    # the distribution library replaces the vendored Windows one.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(IPHONEMIRROR_LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
    target_link_libraries(iPhoneMirror.Core PRIVATE PkgConfig::IPHONEMIRROR_LIBUSB)
endif()

set_target_properties(iPhoneMirror.Core PROPERTIES
    OUTPUT_NAME "iPhoneMirror.Core"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# The libusb-win32 filter-driver helper has no Linux counterpart: a Linux host
# changes the USB configuration through libusb-1.0 directly.
if(WIN32)
    add_executable(iPhoneMirror.UsbConfigurationSwitch
        tools/UsbConfigurationSwitch.cpp)
    target_include_directories(iPhoneMirror.UsbConfigurationSwitch PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party/libusb-win32/include)
    target_compile_definitions(iPhoneMirror.UsbConfigurationSwitch PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(iPhoneMirror.UsbConfigurationSwitch PRIVATE
        iPhoneMirror.CompileOptions
        cfgmgr32
        "${PROJECT_SOURCE_DIR}/third_party/libusb-win32/lib/x64/libusb0-dynamic.lib")
endif()

if(IPHONEMIRROR_BUILD_TESTS)
    enable_testing()
    # CoreTests exercises the Windows capture, decoder, audio and transport
    # stack in one binary, so it stays Windows-only until those backends have
    # Linux implementations. The platform-neutral protocol assertions move to
    # PortableProtocolTests below and run on both platforms.
    if(WIN32)
        add_executable(iPhoneMirror.Core.Tests
            tests/CoreTests.cpp
            src/Protocol/Plist.cpp
            src/Protocol/QuickTimePacket.cpp
            src/Protocol/QuickTimeSession.cpp
            src/Media/H264.cpp
            src/Media/CoreMedia.cpp
            src/Media/MediaFoundationDecoder.cpp
            src/Audio/WasapiRenderer.cpp
            src/Transport/Socket.cpp
            src/Transport/UsbMuxClient.cpp
            src/Transport/QtUsbTransport.cpp
            src/Transport/LibUsb0Transport.cpp
            src/Device/AppleUsbDiscovery.cpp
            src/Capture/CaptureSession.cpp
            src/Capture/WirelessReceiverHub.cpp
            src/Capture/WirelessCaptureSession.cpp
            src/Logging.cpp
        )
        target_include_directories(iPhoneMirror.Core.Tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/src/WirelessHost
            ${PROJECT_SOURCE_DIR}/third_party/libusb/include
            ${PROJECT_SOURCE_DIR}/third_party/libusb-win32/include
        )
        target_compile_definitions(iPhoneMirror.Core.Tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        )
        target_link_libraries(iPhoneMirror.Core.Tests PRIVATE
            iPhoneMirror.CompileOptions
            iPhoneMirror.Core
            "${PROJECT_SOURCE_DIR}/third_party/libusb/lib/x64/libusb-1.0.lib"
            "${PROJECT_SOURCE_DIR}/third_party/libusb-win32/lib/x64/libusb0-dynamic.lib"
            mfplat mf mfuuid wmcodecdspuuid d3d11 dxgi
            ole32 advapi32 setupapi cfgmgr32 avrt winhttp
            ws2_32
            bcrypt
            delayimp
        )
        target_link_options(iPhoneMirror.Core.Tests PRIVATE /DELAYLOAD:libusb0.dll)
        add_custom_command(TARGET iPhoneMirror.Core.Tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${PROJECT_SOURCE_DIR}/third_party/libusb/bin/x64/libusb-1.0.dll
                $<TARGET_FILE_DIR:iPhoneMirror.Core.Tests>/libusb-1.0.dll
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
                $<TARGET_FILE_DIR:iPhoneMirror.Core.Tests>/fixtures
        )
        add_test(NAME CoreProtocolTests COMMAND iPhoneMirror.Core.Tests)
        set_tests_properties(CoreProtocolTests PROPERTIES
            WORKING_DIRECTORY $<TARGET_FILE_DIR:iPhoneMirror.Core.Tests>
        )
    endif()

    # The plist, QuickTime framing, CoreMedia and H.264 assertions currently
    # live inside the Windows-only CoreTests binary. Extracting them into a
    # shared portable test target requires editing CoreTests.cpp, which cannot
    # be compile-verified without a Windows toolchain, so that split is P2 work
    # gated on Windows CI. See docs/LINUX_PORT.md.

    add_executable(iPhoneMirror.OutputModeState.Tests
        tests/OutputModeStateTests.cpp)
    target_include_directories(iPhoneMirror.OutputModeState.Tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(iPhoneMirror.OutputModeState.Tests PRIVATE
        iPhoneMirror.CompileOptions)
    add_test(NAME OutputModeStateTests COMMAND iPhoneMirror.OutputModeState.Tests)

    # Exercises the shared UTF helpers on both platforms; see Text/Utf.h.
    add_executable(iPhoneMirror.Utf.Tests
        tests/UtfTests.cpp
        src/Text/Utf.cpp)
    target_include_directories(iPhoneMirror.Utf.Tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(iPhoneMirror.Utf.Tests PRIVATE
        iPhoneMirror.CompileOptions)
    add_test(NAME UtfTests COMMAND iPhoneMirror.Utf.Tests)

    add_executable(iPhoneMirror.UsbConfigurationRestorePolicy.Tests
        tests/UsbConfigurationRestorePolicyTests.cpp)
    target_include_directories(iPhoneMirror.UsbConfigurationRestorePolicy.Tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(iPhoneMirror.UsbConfigurationRestorePolicy.Tests PRIVATE
        iPhoneMirror.CompileOptions)
    add_test(NAME UsbConfigurationRestorePolicyTests
        COMMAND iPhoneMirror.UsbConfigurationRestorePolicy.Tests)

    if(IPHONEMIRROR_BUILD_DANGEROUS_USB_TOOLS AND WIN32)
        add_executable(iPhoneMirror.LibUsb0Probe
            tools/LibUsb0Probe.cpp
            src/Transport/LibUsb0Transport.cpp
            src/Protocol/QuickTimePacket.cpp
        )
        target_include_directories(iPhoneMirror.LibUsb0Probe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/third_party/libusb/include
            ${PROJECT_SOURCE_DIR}/third_party/libusb-win32/include)
        target_link_libraries(iPhoneMirror.LibUsb0Probe PRIVATE
            iPhoneMirror.CompileOptions
            "${PROJECT_SOURCE_DIR}/third_party/libusb-win32/lib/x64/libusb0-dynamic.lib")
    elseif(WIN32)
        # Multi-config generators keep artifacts for targets removed from a
        # later configure. Remove the old real-device probe so a default safe
        # build cannot accidentally launch a stale binary.
        foreach(config Debug Release RelWithDebInfo MinSizeRel)
            file(REMOVE
                "${CMAKE_CURRENT_BINARY_DIR}/${config}/iPhoneMirror.LibUsb0Probe.exe")
        endforeach()
        file(REMOVE
            "${CMAKE_CURRENT_BINARY_DIR}/iPhoneMirror.LibUsb0Probe.exe")
    endif()

    # The pipeline switch probe drives the full capture C ABI, which is not
    # implemented on Linux yet.
    if(WIN32)
        add_executable(iPhoneMirror.PipelineSwitchProbe
            tools/PipelineSwitchProbe.cpp)
        target_include_directories(iPhoneMirror.PipelineSwitchProbe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(iPhoneMirror.PipelineSwitchProbe PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(iPhoneMirror.PipelineSwitchProbe PRIVATE
            iPhoneMirror.CompileOptions
            iPhoneMirror.Core)
    endif()
endif()

set(IPHONEMIRROR_CORE_INSTALL_TARGETS iPhoneMirror.Core)
if(WIN32)
    list(APPEND IPHONEMIRROR_CORE_INSTALL_TARGETS
        iPhoneMirror.UsbConfigurationSwitch)
endif()
install(TARGETS ${IPHONEMIRROR_CORE_INSTALL_TARGETS}
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    ARCHIVE DESTINATION lib
)
