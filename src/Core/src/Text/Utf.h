// SPDX-License-Identifier: GPL-3.0-only
//
// UTF conversion helpers shared by both platforms. On Windows the upper API
// layers carry wide strings (wchar_t is 16 bits there); the Linux port keeps
// the wide-string ABI (D2 decision) where wchar_t is 32 bits, so both sides
// need the same conversions with their native width.
//
// Windows converts through MultiByteToWideChar/WideCharToMultiByte with the
// default flags, matching the widen()/narrow() helpers the Windows callers
// already use. Linux converts directly; ill-formed input is replaced with
// U+FFFD instead of failing, which mirrors the WinAPI default behaviour of
// accepting and substituting rather than rejecting.

#pragma once

#include <string>
#include <string_view>

namespace iPhoneMirror::text {

[[nodiscard]] std::wstring utf8_to_wide(std::string_view utf8);
[[nodiscard]] std::string wide_to_utf8(std::wstring_view wide);

} // namespace iPhoneMirror::text
