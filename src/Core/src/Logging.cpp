#include "Logging.h"

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <sys/random.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace iPhoneMirror::logging {
namespace {

std::mutex log_mutex;
std::mutex lifecycle_mutex;
std::ofstream log_file;
std::filesystem::path log_path;
bool initialized{};
bool writes_suspended{};
bool session_header_written{};
std::size_t pending_lines{};
std::chrono::steady_clock::time_point last_flush{};
std::jthread flush_thread;
std::uint64_t sequence{};
std::uint64_t line_count{};
std::uint64_t warning_count{};
std::uint64_t error_count{};
std::uint64_t byte_count{};
std::uint64_t dropped_write_count{};
std::string session_id;
std::chrono::steady_clock::time_point session_started{};
std::once_flag fingerprint_salt_once;
std::array<unsigned char, 32> fingerprint_salt{};
bool fingerprint_salt_ready{};

void initialize_fingerprint_salt() noexcept {
#ifdef _WIN32
    fingerprint_salt_ready = BCryptGenRandom(nullptr, fingerprint_salt.data(),
        static_cast<ULONG>(fingerprint_salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    // getrandom blocks only until the kernel pool is initialised, which happens
    // long before any process gets far enough to log a device fingerprint.
    std::size_t filled{};
    while (filled < fingerprint_salt.size()) {
        const auto received = ::getrandom(fingerprint_salt.data() + filled,
            fingerprint_salt.size() - filled, 0);
        if (received <= 0) {
            fingerprint_salt_ready = false;
            return;
        }
        filled += static_cast<std::size_t>(received);
    }
    fingerprint_salt_ready = true;
#endif
}

// The session id and the log header only need the OS process id.
std::uint32_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

const char* level_text(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "INFO";
}

std::string sanitize_single_line(std::string_view value) {
    constexpr std::size_t MaximumMessageBytes = 64U * 1024U;
    const auto length = std::min(value.size(), MaximumMessageBytes);
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character == '\r' || character == '\n' || character == '\t')
            result.push_back(' ');
        else if (character < 0x20 || character == 0x7f)
            result.push_back('?');
        else
            result.push_back(static_cast<char>(character));
    }
    if (value.size() > MaximumMessageBytes) result += "...<truncated>";
    return result;
}

std::string normalize_token(std::string_view value, std::string_view fallback) {
    constexpr std::size_t MaximumTokenBytes = 32;
    std::string result;
    result.reserve(std::min(value.size(), MaximumTokenBytes));
    for (const auto character : value) {
        if (result.size() >= MaximumTokenBytes) break;
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '-')
            result.push_back(static_cast<char>(byte));
        else
            result.push_back('_');
    }
    if (result.empty()) result.assign(fallback);
    return result;
}

std::string infer_category(std::string_view message) {
    const auto first_separator = message.find_first_of(" :");
    const auto token = message.substr(0, first_separator);
    if (token == "ui_event" || token == "action" || token == "diagnostic")
        return "ui";
    const auto underscore = token.find('_');
    return normalize_token(token.substr(0, underscore), "general");
}

Level infer_level(std::string_view message) {
    if (message.starts_with("diagnostic ")) return Level::Debug;
    if (message.starts_with("error ") || message.starts_with("ERROR ") ||
        message.find(" failed") != std::string_view::npos ||
        message.find(" exception") != std::string_view::npos)
        return Level::Error;
    if (message.find(" rejected") != std::string_view::npos ||
        message.find(" dropped") != std::string_view::npos ||
        message.find(" unavailable") != std::string_view::npos)
        return Level::Warning;
    return Level::Info;
}

std::string make_session_id() {
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return std::format("{:08X}-{:016X}", current_process_id(), ticks);
}

std::string now_text();

void flush_pending_locked() {
    if (pending_lines == 0) return;
    const auto lines = pending_lines;
    if (!log_file.is_open() || !log_file.good()) {
        dropped_write_count += lines;
        pending_lines = 0;
        return;
    }
    log_file.flush();
    if (!log_file.good()) {
        dropped_write_count += lines;
    }
    pending_lines = 0;
    last_flush = std::chrono::steady_clock::now();
}

void write_line_locked(Level level, std::string_view category,
    std::string_view message) {
    if (!log_file.is_open() || !log_file.good()) {
        ++dropped_write_count;
        return;
    }
    const auto sanitized_category = normalize_token(category, "general");
    const auto sanitized_message = sanitize_single_line(message);
    const auto line_number = sequence + 1;
    std::ostringstream line;
    line << now_text() << " [tid="
        << std::hash<std::thread::id>{}(std::this_thread::get_id())
        << "] [seq=" << line_number << "] [level=" << level_text(level)
        << "] [category=" << sanitized_category << "] [session="
        << session_id << "] " << sanitized_message << '\n';
    const auto text = line.str();
    log_file << text;
    if (!log_file.good()) {
        // Do not call the logger recursively while handling a disk/handle
        // failure. Leave failbit set so the next logging call can reopen the
        // target, and carry the loss into that recovery marker.
        ++dropped_write_count;
        return;
    }
    sequence = line_number;
    ++line_count;
    if (level == Level::Warning) ++warning_count;
    if (level == Level::Error) ++error_count;
    byte_count += text.size();
    ++pending_lines;
    const auto now = std::chrono::steady_clock::now();
    if (pending_lines >= 64 || now - last_flush >= std::chrono::milliseconds(500)) {
        flush_pending_locked();
    }
}

void ensure_flush_thread_locked() {
    if (flush_thread.joinable()) return;
    flush_thread = std::jthread([](std::stop_token token) {
        // Keep the GUI log tail genuinely live even if the last error is the
        // final line written. Flushing on this background worker avoids disk
        // I/O in the USB/decode/render hot paths.
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::scoped_lock lock(log_mutex);
            flush_pending_locked();
        }
        std::scoped_lock lock(log_mutex);
        flush_pending_locked();
    });
}

std::filesystem::path default_path() {
    // TEMP is writable for both a normal desktop launch and a packaged EXE;
    // unlike the application directory it does not require elevation. On Linux
    // temp_directory_path() resolves TMPDIR or /tmp.
    return std::filesystem::temp_directory_path() / "iPhoneMirror-capture.log";
}

// Returns the configured log path override, or an empty optional when the
// environment variable is unset.
std::optional<std::filesystem::path> log_path_override() {
#ifdef _WIN32
    constexpr auto variable = L"IPHONE_MIRROR_LOG_FILE";
    const auto required = GetEnvironmentVariableW(variable, nullptr, 0);
    if (required == 0) return std::nullopt;

    // Environment variables can approach 32 KiB. Keep that storage off the
    // caller's stack and allocate only as much as this value needs.
    std::wstring buffer(required, L'\0');
    const auto length = GetEnvironmentVariableW(
        variable, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }
    return std::nullopt;
#else
    const char* const value = std::getenv("IPHONE_MIRROR_LOG_FILE");
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::filesystem::path(value);
#endif
}

std::filesystem::path configured_path() {
    if (auto override_path = log_path_override()) return *std::move(override_path);
    return default_path();
}

void rotate_if_needed(const std::filesystem::path& path) {
    constexpr std::uintmax_t MaxLogBytes = 16U * 1024U * 1024U;
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error ||
        std::filesystem::file_size(path, error) <= MaxLogBytes || error) return;
    auto previous = path;
    previous += ".1";
    std::filesystem::remove(previous, error);
    error.clear();
    std::filesystem::rename(path, previous, error);
}

std::string now_text() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << millis;
    return stream.str();
}

bool ensure_session_open_locked(bool implicit) {
    if (!initialized) {
        initialized = true;
        pending_lines = 0;
        sequence = 0;
        line_count = 0;
        warning_count = 0;
        error_count = 0;
        byte_count = 0;
        session_header_written = false;
        session_id = make_session_id();
        session_started = std::chrono::steady_clock::now();
        last_flush = session_started;
    }

    if (log_file.is_open() && log_file.good()) return true;
    if (log_file.is_open()) {
        log_file.clear();
        log_file.close();
    }
    log_file.clear();

    const auto custom_log_path = log_path_override().has_value();
    log_path = configured_path();
    if (!log_path.parent_path().empty())
        std::filesystem::create_directories(log_path.parent_path());
    rotate_if_needed(log_path);
    log_file.open(log_path, std::ios::out | std::ios::app);
    if (!log_file.is_open() || !log_file.good()) {
        if (log_file.is_open()) log_file.close();
        log_file.clear();
        return false;
    }

    const auto dropped_before_open = dropped_write_count;
    if (!session_header_written) {
        log_file << "\n=== iPhoneMirror capture session ===\n";
        log_file << now_text() << " [startup] session=" << session_id
            << " pid=" << current_process_id() << " arch="
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
            << "x64"
#else
            << "x86"
#endif
            << " implicit=" << implicit
            << " log_target=" << (custom_log_path ? "custom" : "default");
        if (dropped_before_open != 0)
            log_file << " dropped_before_start=" << dropped_before_open;
    } else {
        log_file << now_text() << " [log_recovery] session=" << session_id
            << " log_target=" << (custom_log_path ? "custom" : "default")
            << " dropped_before_recovery=" << dropped_before_open;
    }
    log_file << '\n';
    log_file.flush();
    if (!log_file.good()) {
        if (log_file.is_open()) {
            log_file.clear();
            log_file.close();
        }
        log_file.clear();
        return false;
    }

    session_header_written = true;
    dropped_write_count = 0;
    last_flush = std::chrono::steady_clock::now();
    ensure_flush_thread_locked();
    return true;
}

// Hashes the process-local random salt followed by `value`. The salt makes the
// digest useless outside this process, which is the point: logs must be able to
// correlate events for one device without recording its serial.
bool salted_sha256(std::string_view value,
    std::array<unsigned char, 32>& digest) noexcept {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    bool success = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    success = success && BCryptCreateHash(
        algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    success = success && BCryptHashData(hash, fingerprint_salt.data(),
        static_cast<ULONG>(fingerprint_salt.size()), 0) >= 0;
    success = success && BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
        static_cast<ULONG>(value.size()), 0) >= 0;
    success = success && BCryptFinishHash(hash, digest.data(),
        static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
#else
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) return false;
    unsigned int digest_length{};
    const bool success =
        EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(context, fingerprint_salt.data(),
            fingerprint_salt.size()) == 1 &&
        EVP_DigestUpdate(context, value.data(), value.size()) == 1 &&
        EVP_DigestFinal_ex(context, digest.data(), &digest_length) == 1 &&
        digest_length == digest.size();
    EVP_MD_CTX_free(context);
    return success;
#endif
}

} // namespace

std::string fingerprint(std::string_view value) noexcept {
    try {
        if (value.empty()) return "anon-empty";
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            return "anon-too-large";
        std::call_once(fingerprint_salt_once, initialize_fingerprint_salt);
        if (!fingerprint_salt_ready) return "anon-unavailable";

        std::array<unsigned char, 32> digest{};
        if (!salted_sha256(value, digest)) return "anon-unavailable";

        constexpr char hex[] = "0123456789abcdef";
        std::string result = "anon-";
        result.reserve(result.size() + 24);
        for (std::size_t index = 0; index < 12; ++index) {
            result.push_back(hex[digest[index] >> 4]);
            result.push_back(hex[digest[index] & 0x0f]);
        }
        return result;
    } catch (...) {
        return "anon-unavailable";
    }
}

void initialize() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex);
    std::scoped_lock lock(log_mutex);
    writes_suspended = false;
    try {
        (void)ensure_session_open_locked(false);
    } catch (...) {
        initialized = true;
    }
}

void write(std::string_view message) noexcept {
    try {
        std::scoped_lock lock(log_mutex);
        if (writes_suspended) {
            ++dropped_write_count;
            return;
        }
        if (!ensure_session_open_locked(true)) {
            ++dropped_write_count;
            return;
        }
        write_line_locked(infer_level(message), infer_category(message), message);
    } catch (...) {
        // Diagnostics are best-effort and must never alter product behavior.
    }
}

void write(Level level, std::string_view category, std::string_view message) noexcept {
    try {
        std::scoped_lock lock(log_mutex);
        if (writes_suspended) {
            ++dropped_write_count;
            return;
        }
        if (!ensure_session_open_locked(true)) {
            ++dropped_write_count;
            return;
        }
        write_line_locked(level, category, message);
    } catch (...) {
        // Diagnostics are best-effort and must never alter product behavior.
    }
}

void write_event(Level level, std::string_view category, std::string_view event,
    std::string_view details) noexcept {
    try {
        if (details.empty()) {
            write(level, category, event);
            return;
        }
        write(level, category, std::format("event={} {}", event, details));
    } catch (...) {
        // Formatting failures must not escape from a logging call.
    }
}

void shutdown() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex);
    std::jthread worker;
    {
        std::scoped_lock lock(log_mutex);
        // Do this before joining the flush worker. Late writes from API calls
        // already in flight must not lazily reopen the file after shutdown.
        // The next explicit initialize call re-enables writes.
        writes_suspended = true;
        if (!initialized) return;
        worker = std::move(flush_thread);
    }
    if (worker.joinable()) {
        worker.request_stop();
        worker.join();
    }
    {
        std::scoped_lock lock(log_mutex);
        if (log_file.is_open()) {
            const auto elapsed = session_started.time_since_epoch().count() == 0
                ? 0.0
                : std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - session_started).count();
            // A previous write or flush failure leaves failbit set. Clear it
            // for one final best-effort summary, then close the actual handle
            // regardless of whether that summary succeeds.
            log_file.clear();
            log_file << now_text() << " [shutdown] session=" << session_id
                << " lines=" << line_count << " warnings=" << warning_count
                << " errors=" << error_count << " bytes=" << byte_count
                << " elapsed_seconds=" << std::fixed << std::setprecision(3)
                << elapsed << " dropped_after_start=" << dropped_write_count << '\n';
            log_file.flush();
            log_file.close();
        }
        log_file.clear();
        initialized = false;
        session_header_written = false;
        pending_lines = 0;
        session_id.clear();
        session_started = {};
    }
}

} // namespace iPhoneMirror::logging
