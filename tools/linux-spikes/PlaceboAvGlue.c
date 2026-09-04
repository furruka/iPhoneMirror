// SPDX-License-Identifier: GPL-3.0-only
//
// libplacebo's FFmpeg helpers are header-only and their implementation header
// refuses to compile as C++. Following the documented pattern, exactly one C
// translation unit defines PL_LIBAV_IMPLEMENTATION as 1 to emit the external
// definitions, while C++ callers include the same header with
// PL_LIBAV_IMPLEMENTATION set to 0 and link against this object.

#define PL_LIBAV_IMPLEMENTATION 1
#include <libplacebo/utils/libav.h>
