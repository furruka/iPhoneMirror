// SPDX-License-Identifier: GPL-3.0-only
//
// Tests for the shared UTF conversion helpers. The round-trip assertions run
// on both platforms; the substitution assertions pin the documented
// ill-formed-input behaviour of the platform branch being tested.

#include "Text/Utf.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::string_view_literals;

int failures{};

void check(bool condition, std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void round_trip(std::string_view utf8, std::string_view label) {
    const auto wide = iPhoneMirror::text::utf8_to_wide(utf8);
    const auto back = iPhoneMirror::text::wide_to_utf8(wide);
    check(back == utf8,
        std::string(label) + ": round trip preserves the input");
}

} // namespace

int main() {
    // Plain ASCII.
    round_trip("iPhoneMirror"sv, "ascii");

    // Empty input stays empty.
    check(iPhoneMirror::text::utf8_to_wide("").empty(), "empty utf8 -> empty wide");
    check(iPhoneMirror::text::wide_to_utf8(L"").empty(), "empty wide -> empty utf8");

    // Two-byte, three-byte and four-byte sequences, including emoji beyond
    // the BMP (which is exactly where 16-bit and 32-bit wchar_t diverge).
    // String literals are split around hex escapes because \x would otherwise
    // swallow the following hex-digit characters.
    round_trip("Ba\xC3" "\x9F" "che"sv, "german sharp s");
    round_trip("\xE6\x8A\x95" "\xE5\xB1\x8F"sv, "cjk");
    round_trip("\xF0\x9F\x93\xB1"sv, "emoji outside BMP");

    // Embedded NUL and non-ASCII mixed with ASCII.
    round_trip(std::string_view("a\0b\xC3" "\xA9", 5), "embedded nul and accent");

    // Ill-formed sequences substitute instead of crashing or looping forever:
    // truncated lead byte, stray continuation byte and an overlong encoding.
    // The exact number of substitution units is an implementation detail that
    // differs between the WinAPI and the hand-written POSIX decoder, so the
    // cross-platform assertions only bound it.
    const auto truncated = iPhoneMirror::text::utf8_to_wide("\xF0\x9F\x93"sv);
    check(truncated.size() >= 1, "truncated sequence produces output");
    const auto stray = iPhoneMirror::text::utf8_to_wide("a\x80" "b"sv);
    check(stray.size() >= 3 && stray.size() <= 4,
        "stray continuation byte neither disappears nor swallows 'b'");
    const auto overlong = iPhoneMirror::text::utf8_to_wide("\xC0\xAF"sv);
    check(overlong.size() >= 1 && overlong.size() <= 2,
        "overlong encoding is fully consumed");

    // Surrogate halves are not valid UTF-8 and must not pass through.
    const auto surrogate = iPhoneMirror::text::utf8_to_wide("\xED\xA0\x80"sv);
    const auto surrogate_back = iPhoneMirror::text::wide_to_utf8(surrogate);
    check(surrogate_back != "\xED\xA0\x80"sv,
        "utf-8 encoded surrogate is not reproduced verbatim");

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "UtfTests: all checks passed\n";
    return 0;
}
