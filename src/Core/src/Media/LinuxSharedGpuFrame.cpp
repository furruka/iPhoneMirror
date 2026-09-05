// SPDX-License-Identifier: GPL-3.0-only
//
// The Linux side of the one platform hook Media/VideoFrameCopy.cpp needs.
//
// Its own translation unit rather than a few lines in the platform factory file,
// because every target that links VideoFrameCopy.cpp needs this symbol while only
// the library needs the decoder and renderer factories. Folding them together
// would drag PipeWire into tools that never play a sample.

#include "Media/VideoFormats.h"

namespace iPhoneMirror::media::detail {

// The Windows counterpart opens a D3D11 shared texture and reads it back. No
// Linux decoder publishes a cross-device shared GPU frame, so there is never
// anything to materialize; copy_nv12_frame_letterboxed only calls this when
// DecodedFrame::gpu_frame is set.
bool materialize_gpu_frame(DecodedFrame& frame) noexcept {
    return !frame.nv12.empty();
}

} // namespace iPhoneMirror::media::detail
