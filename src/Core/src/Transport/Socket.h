// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <WinSock2.h>

namespace iPhoneMirror::transport {

using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

#else

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace iPhoneMirror::transport {

using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;

#endif

class SocketError final : public std::runtime_error {
public:
    SocketError(const char* operation, int error);
    [[nodiscard]] int code() const noexcept { return code_; }
private:
    int code_;
};

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(SocketHandle handle) noexcept : handle_(handle) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] static Socket connect_loopback(std::uint16_t port, int timeout_ms = 750);
    [[nodiscard]] static bool probe_loopback(std::uint16_t port, int timeout_ms = 250) noexcept;
#ifndef _WIN32
    // Connects to a unix-domain socket such as /var/run/usbmuxd. The timeout
    // bounds the connect() handshake, not later I/O.
    [[nodiscard]] static Socket connect_unix(std::string_view path,
        int timeout_ms = 750);
#endif

    void set_timeout(int timeout_ms);
    void send_all(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::vector<std::uint8_t> receive_exact(std::size_t length);
    [[nodiscard]] std::size_t receive(std::span<std::uint8_t> destination);
    [[nodiscard]] bool shutdown_send_and_wait_for_peer_close(
        int timeout_ms = 500) noexcept;

    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocket; }
    [[nodiscard]] SocketHandle native_handle() const noexcept { return handle_; }
    void close() noexcept;

private:
    SocketHandle handle_{kInvalidSocket};
};

#ifdef _WIN32
void ensure_winsock();
#endif

} // namespace iPhoneMirror::transport
