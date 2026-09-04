// SPDX-License-Identifier: GPL-3.0-only

#include "Text/Utf.h"

#ifdef _WIN32
#include <Windows.h>

#include <vector>
#else
#include <cstdint>
#endif

namespace iPhoneMirror::text {
namespace {

// Replacement character substituted for ill-formed input, matching the W3C
// recommendation and the forgiving behaviour of the WinAPI conversions.
constexpr char32_t ReplacementCharacter = 0xFFFD;

#ifndef _WIN32

// Decodes one UTF-8 sequence. Returns the code point and advances `offset`;
// on ill-formed input consumes exactly one byte and yields U+FFFD, so the
// caller always makes progress and never reads out of bounds.
char32_t decode_utf8(std::string_view utf8, std::size_t& offset) noexcept {
    const auto lead = static_cast<unsigned char>(utf8[offset]);
    ++offset;
    if (lead < 0x80U) return lead;
    if (lead < 0xC2U) return ReplacementCharacter; // continuation or overlong lead

    std::uint32_t value{};
    int remaining{};
    if (lead < 0xE0U) {
        value = lead & 0x1FU;
        remaining = 1;
    } else if (lead < 0xF0U) {
        value = lead & 0x0FU;
        remaining = 2;
    } else if (lead < 0xF5U) {
        value = lead & 0x07U;
        remaining = 3;
    } else {
        return ReplacementCharacter;
    }
    for (int index = 0; index < remaining; ++index) {
        if (offset >= utf8.size()) return ReplacementCharacter;
        const auto continuation = static_cast<unsigned char>(utf8[offset]);
        if ((continuation & 0xC0U) != 0x80U) return ReplacementCharacter;
        value = (value << 6U) | (continuation & 0x3FU);
        ++offset;
    }
    // Reject overlong encodings and surrogate halves.
    if ((remaining == 1 && value < 0x80U) ||
        (remaining == 2 && value < 0x800U) ||
        (remaining == 3 && value < 0x10000U) ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        return ReplacementCharacter;
    }
    return static_cast<char32_t>(value);
}

void append_utf8(std::string& output, char32_t code_point) {
    if (code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU))
        code_point = ReplacementCharacter;
    if (code_point < 0x80U) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000U) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

#endif

} // namespace

#ifdef _WIN32

std::wstring utf8_to_wide(std::string_view utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
            static_cast<int>(utf8.size()), result.data(), length);
    }
    return result;
}

std::string wide_to_utf8(std::wstring_view wide) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(),
            static_cast<int>(wide.size()), result.data(), length,
            nullptr, nullptr);
    }
    return result;
}

#else

std::wstring utf8_to_wide(std::string_view utf8) {
    std::wstring result;
    result.reserve(utf8.size());
    std::size_t offset{};
    while (offset < utf8.size())
        result.push_back(static_cast<wchar_t>(decode_utf8(utf8, offset)));
    return result;
}

std::string wide_to_utf8(std::wstring_view wide) {
    std::string result;
    result.reserve(wide.size() * 4U);
    for (const wchar_t unit : wide)
        append_utf8(result, static_cast<char32_t>(unit));
    return result;
}

#endif

} // namespace iPhoneMirror::text
