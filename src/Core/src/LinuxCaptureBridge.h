// SPDX-License-Identifier: GPL-3.0-only
//
// Internal bridge between the Linux capture exports and the Linux preview
// exports, which live in separate translation units on purpose: the capture side
// owns the session, the preview side owns the renderer, and neither needs the
// other's headers.
//
// The accessor returns the frame rather than the session because the session
// pointer is only valid while the capture mutex is held; handing a raw pointer
// across would be a use-after-free waiting for a concurrent im_stop_capture.

#pragma once

#include "Media/VideoFormats.h"

#include <memory>

namespace iPhoneMirror::linux_bridge {

// Null when no session is running or no frame has arrived yet.
[[nodiscard]] std::shared_ptr<const media::DecodedFrame> latest_render_frame();

} // namespace iPhoneMirror::linux_bridge
