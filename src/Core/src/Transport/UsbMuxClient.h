#pragma once

#include "Protocol/Plist.h"
#include "Transport/Socket.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iPhoneMirror::transport {

// The canonical usbmuxd unix-domain socket on Linux. Apple's Windows service
// listens on loopback TCP instead, which is why the client keeps both.
inline constexpr std::string_view UsbMuxUnixSocketPath = "/var/run/usbmuxd";

struct MuxDevice {
    std::uint32_t device_id{};
    std::string serial;
    std::string connection_type;
    std::uint32_t product_id{};
    std::uint32_t location_id{};
};

class UsbMuxClient {
public:
    explicit UsbMuxClient(std::uint16_t port) : port_(port) {}
#ifndef _WIN32
    // Connects over AF_UNIX instead of loopback TCP. port() then reports 0,
    // which is how the diagnostics distinguish the two endpoints.
    explicit UsbMuxClient(std::string unix_socket_path)
        : unix_socket_path_(std::move(unix_socket_path)) {}
#endif

    [[nodiscard]] std::vector<MuxDevice> list_devices();
    [[nodiscard]] bool has_pair_record(const std::string& udid);
    [[nodiscard]] Socket connect_device(std::uint32_t device_id, std::uint16_t device_port);
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    std::uint16_t port_{};
#ifndef _WIN32
    std::string unix_socket_path_;
#endif
    std::uint32_t next_tag_{1};

    [[nodiscard]] Socket connect();
    [[nodiscard]] plist::Value request(const plist::Value& body);
    [[nodiscard]] std::pair<Socket, plist::Value> request_with_socket(const plist::Value& body);
    [[nodiscard]] plist::Value base_message(std::string message_type) const;
};

} // namespace iPhoneMirror::transport

