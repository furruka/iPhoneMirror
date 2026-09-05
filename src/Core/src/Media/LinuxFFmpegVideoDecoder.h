// SPDX-License-Identifier: GPL-3.0-only
//
// Linux implementation of the IVideoDecoder seam, built on libavcodec.
//
// The factory is separate from make_platform_video_decoder so the decoder can be
// constructed directly by tests and by the acceptance tool without going through
// the platform selection in Media/ActiveVideoDecoder.h.

#pragma once

#include "Media/IVideoDecoder.h"

#include <memory>

namespace iPhoneMirror::media {

// Throws std::runtime_error when libavcodec has no decoder for the codec the
// caller later configures, or when the codec context cannot be opened. Never
// returns null.
[[nodiscard]] std::unique_ptr<IVideoDecoder> make_ffmpeg_video_decoder(
    DecoderPreference preference);

} // namespace iPhoneMirror::media
