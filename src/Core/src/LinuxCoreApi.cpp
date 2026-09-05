// SPDX-License-Identifier: GPL-3.0-only
//
// Linux implementation of the Core C ABI. Written as a new file rather than by
// editing CoreApi.cpp, which is built entirely around Windows session,
// preview-HWND, virtual-camera and AirPlay plumbing.
//
// What is real here: initialization, logging, the environment report and device
// enumeration. That is the WP3 scope and it is what the Linux shell needs to
// show a device list and explain why capture is or is not possible.
//
// Everything else returns an explicit failure with a message naming the work
// package that implements it. The rule is that no export may report success
// without having done the thing: a caller that gets Ok from im_start_capture
// would wait forever for frames.

#include "iPhoneMirror/CoreApi.h"

#include "Device/DeviceManager.h"
#include "Logging.h"
#include "Text/Utf.h"

#include <algorithm>
#include <mutex>
#include <string>

namespace {

std::mutex state_mutex;
bool initialized{};
std::wstring last_error;
iPhoneMirror::device::DeviceManager device_manager;

std::int32_t fail(iPhoneMirror::Result result, std::wstring message) {
    last_error = std::move(message);
    return static_cast<std::int32_t>(result);
}

// Reports a Linux export that has no implementation yet. The message names the
// work package so a caller sees why rather than a bare error code.
std::int32_t not_implemented(std::wstring_view feature, std::wstring_view work_package) {
    last_error = std::wstring(feature) + L" 在 Linux 版尚未实现（" +
        std::wstring(work_package) + L"）";
    return static_cast<std::int32_t>(
        iPhoneMirror::Result::CaptureBackendUnavailable);
}

template <std::size_t Capacity>
void copy_text(wchar_t (&destination)[Capacity], std::wstring_view source) {
    const auto length = std::min(source.size(), Capacity - 1);
    std::copy_n(source.begin(), length, destination);
    destination[length] = L'\0';
}

std::int32_t refresh_devices_locked(iPhoneMirror::DeviceInfo* devices,
    std::uint32_t* count, bool refresh_metadata) {
    if (!count) {
        return fail(iPhoneMirror::Result::InvalidArgument, L"count 不能为空");
    }
    std::scoped_lock lock(state_mutex);
    if (!initialized) {
        return fail(iPhoneMirror::Result::NotInitialized, L"核心尚未初始化");
    }
    try {
        const auto records = device_manager.refresh(refresh_metadata);
        const auto capacity = *count;
        *count = static_cast<std::uint32_t>(records.size());
        if (!devices) {
            last_error.clear();
            return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
        }
        if (capacity < records.size()) {
            return fail(iPhoneMirror::Result::BufferTooSmall,
                L"DeviceInfo 缓冲区容量不足");
        }
        for (std::size_t index = 0; index < records.size(); ++index) {
            const auto& record = records[index];
            auto& info = devices[index];
            info.struct_size = sizeof(iPhoneMirror::DeviceInfo);
            info.api_version = iPhoneMirror::ApiVersion;
            info.device_id = record.device_id;
            info.mux_port = record.mux_port;
            info.state = record.state;
            info.usb_connected = record.usb_connected ? 1 : 0;
            info.pair_record_present = record.pair_record_present ? 1 : 0;
            info.lockdown_accessible = record.lockdown_accessible ? 1 : 0;
            copy_text(info.udid, record.udid);
            copy_text(info.name, record.name);
            copy_text(info.product_type, record.product_type);
            copy_text(info.os_version, record.os_version);
            copy_text(info.connection_type, record.connection_type);
            copy_text(info.status, record.status);
        }
        last_error.clear();
        return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
    } catch (const std::exception& error) {
        return fail(iPhoneMirror::Result::InternalError,
            L"枚举设备失败：" + iPhoneMirror::text::utf8_to_wide(error.what()));
    } catch (...) {
        return fail(iPhoneMirror::Result::InternalError, L"枚举设备时发生异常");
    }
}

} // namespace

std::int32_t IM_CALL im_initialize() {
    std::scoped_lock lock(state_mutex);
    iPhoneMirror::logging::initialize();
    initialized = true;
    last_error.clear();
    iPhoneMirror::logging::write("core initialize platform=linux");
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

void IM_CALL im_shutdown() {
    std::scoped_lock lock(state_mutex);
    if (!initialized) return;
    initialized = false;
    iPhoneMirror::logging::write("core shutdown platform=linux");
    iPhoneMirror::logging::shutdown();
}

std::uint32_t IM_CALL im_api_version() { return iPhoneMirror::ApiVersion; }

std::int32_t IM_CALL im_log_message(const wchar_t* message) {
    if (!message || !*message) {
        return fail(iPhoneMirror::Result::InvalidArgument, L"日志消息不能为空");
    }
    constexpr std::size_t MaxLogMessage = 4096;
    std::wstring_view text(message);
    if (text.size() > MaxLogMessage) {
        return fail(iPhoneMirror::Result::InvalidArgument, L"日志消息过长");
    }
    iPhoneMirror::logging::write(iPhoneMirror::text::wide_to_utf8(text));
    last_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

std::int32_t IM_CALL im_get_environment(iPhoneMirror::EnvironmentInfo* environment) {
    if (!environment ||
        environment->struct_size != sizeof(iPhoneMirror::EnvironmentInfo)) {
        return fail(iPhoneMirror::Result::InvalidArgument,
            L"EnvironmentInfo 结构版本不匹配");
    }
    std::scoped_lock lock(state_mutex);
    if (!initialized) {
        return fail(iPhoneMirror::Result::NotInitialized, L"核心尚未初始化");
    }
    try {
        const auto info = device_manager.environment();
        environment->api_version = iPhoneMirror::ApiVersion;
        environment->apple_mobile_device_service_installed = info.service_installed;
        environment->apple_mobile_device_service_running = info.service_running;
        environment->standard_usbmux_available = info.standard_mux;
        environment->capture_usbmux_available = info.capture_mux;
        environment->physical_apple_usb_devices = info.physical_device_count;
        copy_text(environment->diagnostic, info.diagnostic);
        environment->libusb_runtime_available = info.libusb_runtime;
        environment->usbdk_backend_available = info.usbdk_backend;
        environment->libusb_apple_devices = info.libusb_apple_devices;
        copy_text(environment->libusb_version, info.libusb_version);
        environment->usbdk_backend_known = info.usbdk_backend_known;
        environment->libusb_apple_devices_known = info.libusb_apple_devices_known;
        last_error.clear();
        return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
    } catch (...) {
        return fail(iPhoneMirror::Result::InternalError, L"读取驱动环境时发生异常");
    }
}

std::int32_t IM_CALL im_refresh_devices(iPhoneMirror::DeviceInfo* devices,
    std::uint32_t* count) {
    return refresh_devices_locked(devices, count, false);
}

std::int32_t IM_CALL im_refresh_devices_ex(iPhoneMirror::DeviceInfo* devices,
    std::uint32_t* count, std::int32_t refresh_metadata) {
    return refresh_devices_locked(devices, count, refresh_metadata != 0);
}

const wchar_t* IM_CALL im_last_error() { return last_error.c_str(); }

// Wired USB capture. The transport, the QuickTime handshake and the shared
// capture state machine all exist on Linux; what is missing is the FFmpeg
// decoder and the PipeWire renderer behind the media seams, so starting a
// session would produce a stream with nowhere to decode it.
std::int32_t IM_CALL im_start_capture(const wchar_t*) {
    return not_implemented(L"有线投屏", L"WP4/WP5");
}

std::int32_t IM_CALL im_start_capture_ex(const wchar_t*, std::int32_t) {
    return not_implemented(L"有线投屏", L"WP4/WP5");
}

std::int32_t IM_CALL im_start_capture_with_options(const wchar_t*,
    const iPhoneMirror::CaptureOptions*) {
    return not_implemented(L"有线投屏", L"WP4/WP5");
}

std::int32_t IM_CALL im_stop_capture() {
    return not_implemented(L"有线投屏", L"WP4/WP5");
}

std::int32_t IM_CALL im_get_capture_status(iPhoneMirror::CaptureStatus* status) {
    if (!status || status->struct_size != sizeof(iPhoneMirror::CaptureStatus)) {
        return fail(iPhoneMirror::Result::InvalidArgument,
            L"CaptureStatus 结构版本不匹配");
    }
    // A well-formed idle status is the honest answer: no session can exist yet.
    status->api_version = iPhoneMirror::ApiVersion;
    status->state = iPhoneMirror::CaptureState::Idle;
    status->width = 0;
    status->height = 0;
    status->fps = 0.0;
    status->latency_ms = 0.0;
    status->video_frames = 0;
    status->audio_packets = 0;
    status->audio_sample_rate = 0;
    status->audio_channels = 0;
    status->failure_kind = iPhoneMirror::CaptureFailureKind::None;
    status->failure_stage = iPhoneMirror::CaptureFailureStage::None;
    status->error_code = 0;
    copy_text(status->message, L"Linux 版尚未实现采集（WP4/WP5）");
    last_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

std::int32_t IM_CALL im_get_latest_video_timestamp(std::int64_t* timestamp_100ns) {
    if (!timestamp_100ns) {
        return fail(iPhoneMirror::Result::InvalidArgument, L"timestamp 不能为空");
    }
    *timestamp_100ns = 0;
    last_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

std::int32_t IM_CALL im_copy_latest_video_frame(iPhoneMirror::VideoFrameInfo*,
    std::uint8_t*, std::uint32_t*) {
    return not_implemented(L"视频帧拷贝", L"WP5");
}

std::int32_t IM_CALL im_copy_latest_video_frame_scaled(
    iPhoneMirror::VideoFrameInfo*, std::uint8_t*, std::uint32_t*,
    std::uint32_t, std::uint32_t) {
    return not_implemented(L"视频帧拷贝", L"WP5");
}

// Preview hosting. Linux uses the exported-target model from decision D1
// (libplacebo renders into an exportable image that Avalonia imports), so the
// HWND-shaped entry points never gain a Linux implementation.
std::int32_t IM_CALL im_attach_preview_window(void*) {
    return not_implemented(L"HWND 预览宿主", L"Linux 改用导出面模型，见 D1");
}

void IM_CALL im_detach_preview_window() {}

std::int32_t IM_CALL im_force_preview_refresh() {
    return not_implemented(L"预览刷新", L"WP5");
}

std::int32_t IM_CALL im_set_preview_corner_profile(float, float) {
    return not_implemented(L"预览圆角裁剪", L"WP6");
}

std::int32_t IM_CALL im_set_video_preferences(std::uint32_t, std::uint32_t,
    std::uint32_t) {
    return not_implemented(L"视频渲染偏好", L"WP5");
}

std::int32_t IM_CALL im_set_image_adjustments(float, float, float, float) {
    return not_implemented(L"图像调节", L"WP5");
}

std::int32_t IM_CALL im_set_audio_enabled(std::int32_t) {
    return not_implemented(L"音频开关", L"WP5");
}

std::int32_t IM_CALL im_set_audio_volume(float) {
    return not_implemented(L"音量设置", L"WP5");
}

std::int32_t IM_CALL im_is_libusb0_device_available(const wchar_t*,
    std::int32_t* available) {
    if (!available) {
        return fail(iPhoneMirror::Result::InvalidArgument, L"available 不能为空");
    }
    // libusb0 is the Windows libusb-win32 filter backend. It does not exist on
    // Linux, so the answer is a definite "no", not a failure.
    *available = 0;
    last_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

// Multi-device sessions. The shared capture state machine is ready; these wait
// on the same decoder/renderer work as im_start_capture.
std::int32_t IM_CALL im_session_create(const wchar_t*,
    const iPhoneMirror::CaptureOptions*, iPhoneMirror::SessionHandle* handle) {
    if (handle) *handle = 0;
    return not_implemented(L"有线采集会话", L"WP4/WP5");
}

std::int32_t IM_CALL im_session_stop(iPhoneMirror::SessionHandle) {
    return not_implemented(L"有线采集会话", L"WP4/WP5");
}

void IM_CALL im_session_destroy(iPhoneMirror::SessionHandle) {}

std::int32_t IM_CALL im_session_get_status(iPhoneMirror::SessionHandle,
    iPhoneMirror::CaptureStatus*) {
    return not_implemented(L"会话状态查询", L"WP4/WP5");
}

std::int32_t IM_CALL im_session_get_video_output_status(
    iPhoneMirror::SessionHandle, void*, iPhoneMirror::VideoOutputStatus*) {
    return not_implemented(L"视频输出诊断", L"WP5");
}

std::int32_t IM_CALL im_session_attach_preview(iPhoneMirror::SessionHandle, void*) {
    return not_implemented(L"HWND 预览宿主", L"Linux 改用导出面模型，见 D1");
}

void IM_CALL im_session_detach_preview(iPhoneMirror::SessionHandle, void*) {}

std::int32_t IM_CALL im_session_set_video_preferences(iPhoneMirror::SessionHandle,
    std::uint32_t, std::uint32_t, std::uint32_t) {
    return not_implemented(L"视频渲染偏好", L"WP5");
}

std::int32_t IM_CALL im_session_set_image_adjustments(iPhoneMirror::SessionHandle,
    float, float, float, float) {
    return not_implemented(L"图像调节", L"WP5");
}

std::int32_t IM_CALL im_session_set_pipeline_preferences(
    iPhoneMirror::SessionHandle, std::uint32_t, std::uint32_t) {
    return not_implemented(L"解码/色彩策略", L"WP5");
}

std::int32_t IM_CALL im_session_set_audio_enabled(iPhoneMirror::SessionHandle,
    std::int32_t) {
    return not_implemented(L"音频开关", L"WP5");
}

std::int32_t IM_CALL im_session_set_audio_volume(iPhoneMirror::SessionHandle, float) {
    return not_implemented(L"音量设置", L"WP5");
}

std::int32_t IM_CALL im_session_set_corner_profile(iPhoneMirror::SessionHandle,
    float, float) {
    return not_implemented(L"预览圆角裁剪", L"WP6");
}

std::int32_t IM_CALL im_session_get_latest_video_timestamp(
    iPhoneMirror::SessionHandle, std::int64_t*) {
    return not_implemented(L"会话视频时间戳", L"WP5");
}

std::int32_t IM_CALL im_session_copy_latest_video_frame(iPhoneMirror::SessionHandle,
    iPhoneMirror::VideoFrameInfo*, std::uint8_t*, std::uint32_t*,
    std::uint32_t, std::uint32_t) {
    return not_implemented(L"视频帧拷贝", L"WP5");
}

std::int32_t IM_CALL im_session_copy_latest_video_frame_nv12(
    iPhoneMirror::SessionHandle, iPhoneMirror::VideoFrameInfo*, std::uint8_t*,
    std::uint32_t*, std::uint32_t, std::uint32_t) {
    return not_implemented(L"NV12 帧拷贝", L"WP5");
}

std::int32_t IM_CALL im_session_copy_next_audio_packet(iPhoneMirror::SessionHandle,
    std::uint64_t, iPhoneMirror::AudioPacketInfo*, std::uint8_t*, std::uint32_t*) {
    return not_implemented(L"PCM 包拷贝", L"WP5");
}

std::int32_t IM_CALL im_session_force_preview_refresh(iPhoneMirror::SessionHandle) {
    return not_implemented(L"预览刷新", L"WP5");
}

std::int32_t IM_CALL im_session_set_window_corner_profile(
    iPhoneMirror::SessionHandle, void*, float, float) {
    return not_implemented(L"预览圆角裁剪", L"WP6");
}

std::int32_t IM_CALL im_session_set_window_rotation(iPhoneMirror::SessionHandle,
    void*, std::int32_t) {
    return not_implemented(L"预览旋转", L"WP6");
}

// AirPlay receiver. The Linux receiver is built from UxPlay sources rather than
// the vendored AirPlayServer runtime, which is a separate work package.
std::int32_t IM_CALL im_wireless_receiver_start(const wchar_t*, const wchar_t*) {
    return not_implemented(L"AirPlay 接收端", L"WP6 之后的 P6");
}

std::int32_t IM_CALL im_wireless_receiver_start_ex(const wchar_t*, const wchar_t*,
    std::uint32_t, std::uint32_t, std::uint32_t) {
    return not_implemented(L"AirPlay 接收端", L"WP6 之后的 P6");
}

void IM_CALL im_wireless_receiver_stop() {}

std::int32_t IM_CALL im_wireless_receiver_get_status(std::int32_t* running,
    std::int32_t* ready) {
    if (running) *running = 0;
    if (ready) *ready = 0;
    last_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

std::int32_t IM_CALL im_refresh_wireless_devices(iPhoneMirror::DeviceInfo*,
    std::uint32_t* count) {
    // No receiver can be running, so an empty list is the correct answer.
    if (!count) {
        return fail(iPhoneMirror::Result::InvalidArgument, L"count 不能为空");
    }
    *count = 0;
    last_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

std::int32_t IM_CALL im_wireless_session_create(const wchar_t*,
    const iPhoneMirror::CaptureOptions*, iPhoneMirror::SessionHandle* handle) {
    if (handle) *handle = 0;
    return not_implemented(L"AirPlay 采集会话", L"WP6 之后的 P6");
}

std::int32_t IM_CALL im_media_cast_receiver_start(const wchar_t*, const wchar_t*) {
    return not_implemented(L"URL 视频接收端", L"WP6 之后的 P6");
}

void IM_CALL im_media_cast_receiver_stop() {}

std::int32_t IM_CALL im_media_cast_receiver_get_status(std::int32_t* running,
    std::int32_t* ready) {
    if (running) *running = 0;
    if (ready) *ready = 0;
    last_error.clear();
    return static_cast<std::int32_t>(iPhoneMirror::Result::Ok);
}

std::int32_t IM_CALL im_media_cast_get_request(iPhoneMirror::MediaCastRequest*) {
    return not_implemented(L"URL 视频请求", L"WP6 之后的 P6");
}

std::int32_t IM_CALL im_media_cast_set_playback_state(std::uint64_t, double,
    double, double) {
    return not_implemented(L"URL 视频播放状态", L"WP6 之后的 P6");
}

std::int32_t IM_CALL im_media_cast_request_stop() {
    return not_implemented(L"URL 视频停止", L"WP6 之后的 P6");
}


