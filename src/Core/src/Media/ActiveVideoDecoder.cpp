// SPDX-License-Identifier: GPL-3.0-only

#include "Media/ActiveVideoDecoder.h"
#include "Media/MediaFoundationDecoder.h"
#include "Audio/WasapiRenderer.h"

namespace iPhoneMirror::media {

std::unique_ptr<IVideoDecoder> make_platform_video_decoder(
    DecoderPreference preference) {
    return std::make_unique<MediaFoundationVideoDecoder>(preference);
}

} // namespace iPhoneMirror::media

namespace iPhoneMirror::audio {

std::unique_ptr<IAudioRenderer> make_platform_audio_renderer(
    const coremedia::AudioStreamBasicDescription& format,
    bool playback_enabled, float volume) {
    return std::make_unique<WasapiRenderer>(format, playback_enabled, volume);
}

} // namespace iPhoneMirror::audio
