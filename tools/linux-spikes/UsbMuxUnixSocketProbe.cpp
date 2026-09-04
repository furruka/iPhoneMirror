// SPDX-License-Identifier: GPL-3.0-only
//
// Linux port spike S2: usbmux over AF_UNIX.
//
// The Windows core reaches Apple Mobile Device Support over TCP loopback ports
// 27015/37015. On Linux the same plist protocol is served by usbmuxd on the
// Unix socket /var/run/usbmuxd. This spike proves the wire protocol in
// src/Core/src/Transport/UsbMuxClient.cpp is byte-compatible with usbmuxd by
// reusing this project's own plist parser and packet framing rules.
//
// The probe is read-only: it sends a single ListDevices request and prints the
// reply. It never connects to a device port and never changes device state.

#include "Protocol/Plist.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Mirrors the constants in src/Core/src/Transport/UsbMuxClient.cpp so a
// mismatch here is a real protocol mismatch rather than a divergent copy.
constexpr std::uint32_t PlistMessage = 8;
constexpr std::uint32_t ProtocolVersion = 1;
constexpr std::uint32_t MaxMuxPacket = 16U * 1024U * 1024U;
constexpr char DefaultSocketPath[] = "/var/run/usbmuxd";

void append_u32le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t u32le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

class UnixSocket {
public:
    ~UnixSocket() { if (descriptor_ >= 0) ::close(descriptor_); }
    UnixSocket() = default;
    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;

    [[nodiscard]] bool connect(const char* path) {
        descriptor_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor_ < 0) return false;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto length = std::strlen(path);
        if (length >= sizeof(address.sun_path)) return false;
        std::memcpy(address.sun_path, path, length);
        return ::connect(descriptor_, reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) == 0;
    }

    [[nodiscard]] bool send_all(const std::vector<std::uint8_t>& bytes) const {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const auto written = ::send(descriptor_, bytes.data() + sent,
                bytes.size() - sent, 0);
            if (written <= 0) return false;
            sent += static_cast<std::size_t>(written);
        }
        return true;
    }

    [[nodiscard]] bool receive_exact(std::uint8_t* destination,
        std::size_t length) const {
        std::size_t received = 0;
        while (received < length) {
            const auto read = ::recv(descriptor_, destination + received,
                length - received, 0);
            if (read <= 0) return false;
            received += static_cast<std::size_t>(read);
        }
        return true;
    }

private:
    int descriptor_{-1};
};

void print_device(const iPhoneMirror::plist::Value& entry) {
    const auto* properties = entry.find("Properties");
    if (properties == nullptr) return;

    const auto serial = properties->find("SerialNumber");
    const auto connection = properties->find("ConnectionType");
    const auto location = properties->find("LocationID");
    const auto product = properties->find("ProductID");
    const auto device_id = entry.find("DeviceID");

    std::printf("  device_id=%lld udid=%s connection=%s product_id=0x%04llx "
        "location=0x%llx\n",
        static_cast<long long>(device_id != nullptr ? device_id->integer_or() : 0),
        serial != nullptr ? serial->string_or("?").c_str() : "?",
        connection != nullptr ? connection->string_or("?").c_str() : "?",
        static_cast<unsigned long long>(
            product != nullptr ? product->integer_or() : 0),
        static_cast<unsigned long long>(
            location != nullptr ? location->integer_or() : 0));
}

} // namespace

int main(int argc, char** argv) {
    const char* socket_path = argc > 1 ? argv[1] : DefaultSocketPath;

    std::printf("Linux port spike S2: usbmux ListDevices over AF_UNIX\n");
    std::printf("socket        : %s\n", socket_path);

    UnixSocket socket;
    if (!socket.connect(socket_path)) {
        std::fprintf(stderr, "cannot connect to %s: %s\n", socket_path,
            std::strerror(errno));
        std::fprintf(stderr,
            "hint: start usbmuxd (systemctl start usbmuxd) and retry\n");
        return 2;
    }
    std::printf("connect       : ok\n");

    // Same message shape as UsbMuxClient::base_message.
    const auto body = iPhoneMirror::plist::Value::Dict({
        {"BundleID", iPhoneMirror::plist::Value::String("com.iphonemirror.linux")},
        {"ClientVersionString",
            iPhoneMirror::plist::Value::String("iPhoneMirror linux-port spike")},
        {"MessageType", iPhoneMirror::plist::Value::String("ListDevices")},
        {"ProgName", iPhoneMirror::plist::Value::String("iPhoneMirror")},
        {"kLibUSBMuxVersion", iPhoneMirror::plist::Value::Integer(3)},
    });

    const std::string xml = iPhoneMirror::plist::to_xml(body);
    std::vector<std::uint8_t> packet;
    packet.reserve(16 + xml.size());
    append_u32le(packet, static_cast<std::uint32_t>(16 + xml.size()));
    append_u32le(packet, ProtocolVersion);
    append_u32le(packet, PlistMessage);
    append_u32le(packet, 1);
    packet.insert(packet.end(), xml.begin(), xml.end());

    if (!socket.send_all(packet)) {
        std::fprintf(stderr, "send failed: %s\n", std::strerror(errno));
        return 2;
    }
    std::printf("request       : ListDevices (%zu bytes)\n", packet.size());

    std::array<std::uint8_t, 16> header{};
    if (!socket.receive_exact(header.data(), header.size())) {
        std::fprintf(stderr, "no reply header: %s\n", std::strerror(errno));
        return 2;
    }

    const auto total = u32le(header.data());
    const auto version = u32le(header.data() + 4);
    const auto message = u32le(header.data() + 8);
    const auto tag = u32le(header.data() + 12);
    std::printf("reply header  : length=%u version=%u message=%u tag=%u\n",
        total, version, message, tag);

    if (total < header.size() || total > MaxMuxPacket) {
        std::fprintf(stderr, "reply length %u is out of bounds\n", total);
        return 3;
    }
    if (version != ProtocolVersion || message != PlistMessage) {
        std::fprintf(stderr,
            "unexpected framing: version=%u message=%u (expected %u/%u)\n",
            version, message, ProtocolVersion, PlistMessage);
        return 3;
    }

    std::vector<std::uint8_t> payload(total - header.size());
    if (!payload.empty() && !socket.receive_exact(payload.data(), payload.size())) {
        std::fprintf(stderr, "truncated reply payload\n");
        return 3;
    }

    try {
        const auto reply = iPhoneMirror::plist::parse_xml(
            std::string_view(reinterpret_cast<const char*>(payload.data()),
                payload.size()));
        std::printf("plist parse   : ok (this project's parser accepted usbmuxd's reply)\n");

        const auto* device_list = reply.find("DeviceList");
        if (device_list == nullptr) {
            std::printf("devices       : DeviceList key absent\n");
            return 0;
        }
        std::printf("devices       : %zu\n", device_list->array.size());
        for (const auto& entry : device_list->array) print_device(entry);
        if (device_list->array.empty())
            std::printf("  (connect an unlocked, trusted iPhone or iPad and retry)\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "plist parse failed: %s\n", error.what());
        return 3;
    }

    return 0;
}
