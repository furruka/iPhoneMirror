// SPDX-License-Identifier: GPL-3.0-only
//
// Linux platform selection for the capture session's media seams. The backends
// themselves live in Media/LinuxFFmpegVideoDecoder.cpp and
// Audio/LinuxPipeWireAudioRenderer.cpp; this file only decides which one the
// platform build hands to CaptureSession.

#include "Media/ActiveVideoDecoder.h"

#include "Audio/LinuxPipeWireAudioRenderer.h"
#include "Media/CoreMedia.h"
#include "Media/LinuxFFmpegVideoDecoder.h"

namespace iPhoneMirror::media {

std::unique_ptr<IVideoDecoder> make_platform_video_decoder(
    DecoderPreference preference) {
    return make_ffmpeg_video_decoder(preference);
}

} // namespace iPhoneMirror::media

namespace iPhoneMirror::audio {

std::unique_ptr<IAudioRenderer> make_platform_audio_renderer(
    const coremedia::AudioStreamBasicDescription& format,
    bool playback_enabled, float volume) {
    return make_pipewire_audio_renderer(format, playback_enabled, volume);
}

} // namespace iPhoneMirror::audio
