// SPDX-License-Identifier: GPL-3.0-only
//
// WP4 headless wired capture. Writes the raw elementary stream and PCM the
// device sends to disk and nothing else: no decoder, no renderer, no window.
// That is deliberate. It makes the USB half verifiable on its own, before the
// FFmpeg decoder and the PipeWire renderer exist, and a file that plays in any
// player is stronger evidence than a preview window.
//
// The interesting part is the middle: after the 0x52 vendor request the device
// detaches and udev's 39-usbmuxd.rules writes bConfigurationValue back to 0 on
// the new add event, so the host has to re-issue SET_CONFIGURATION itself. The
// sequencing lives in Capture/UsbReenumerationPolicy.h and the observation comes
// from Device/LinuxUdevMonitor.h; this tool wires them to the real transport.
//
// It changes USB configuration state, so it is opt-in through
// IPHONEMIRROR_BUILD_DANGEROUS_USB_TOOLS and always restores the normal
// configuration on the way out.

#include "Capture/UsbReenumerationPolicy.h"
#include "Device/LinuxUdevMonitor.h"
#include "Logging.h"
#include "Media/H264.h"
#include "Protocol/QuickTimePacket.h"
#include "Protocol/QuickTimeSession.h"
#include "Transport/AppleUsbSerial.h"
#include "Transport/LinuxUsbConfiguration.h"
#include "Transport/QtUsbTransport.h"

#include <chrono>
#include <format>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace iPhoneMirror;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string serial;
    std::string video_path{"/tmp/ipm_wp4/capture.h264"};
    std::string audio_path{"/tmp/ipm_wp4/capture.wav"};
    std::chrono::seconds duration{15};
    bool verbose{};
};

void print_usage() {
    std::fprintf(stderr,
        "usage: iPhoneMirror.Linux.HeadlessCapture --serial <udid> "
        "[--video <path.h264>] [--audio <path.wav>] [--seconds N] [--verbose]\n");
}

std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto next = [&]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (argument == "--serial") {
            const auto* value = next();
            if (value == nullptr) return std::nullopt;
            options.serial = value;
        } else if (argument == "--video") {
            const auto* value = next();
            if (value == nullptr) return std::nullopt;
            options.video_path = value;
        } else if (argument == "--audio") {
            const auto* value = next();
            if (value == nullptr) return std::nullopt;
            options.audio_path = value;
        } else if (argument == "--seconds") {
            const auto* value = next();
            if (value == nullptr) return std::nullopt;
            options.duration = std::chrono::seconds(std::atoi(value));
            if (options.duration.count() <= 0) return std::nullopt;
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else {
            return std::nullopt;
        }
    }
    if (options.serial.empty()) return std::nullopt;
    return options;
}

// Minimal RIFF/WAVE writer. The header is written twice: once with placeholder
// sizes so the stream can be appended, and again on close with the real values.
class WavWriter final {
public:
    bool open(const std::string& path, std::uint32_t sample_rate,
        std::uint16_t channels, std::uint16_t bits_per_sample) {
        sample_rate_ = sample_rate;
        channels_ = channels;
        bits_per_sample_ = bits_per_sample;
        stream_.open(path, std::ios::binary | std::ios::trunc);
        if (!stream_) return false;
        write_header();
        return true;
    }

    void append(std::span<const std::uint8_t> pcm) {
        if (!stream_) return;
        stream_.write(reinterpret_cast<const char*>(pcm.data()),
            static_cast<std::streamsize>(pcm.size()));
        data_bytes_ += pcm.size();
    }

    void close() {
        if (!stream_) return;
        stream_.seekp(0, std::ios::beg);
        write_header();
        stream_.close();
    }

    [[nodiscard]] std::size_t data_bytes() const noexcept { return data_bytes_; }
    [[nodiscard]] bool is_open() const noexcept { return stream_.is_open(); }

private:
    void put32(std::uint32_t value) {
        const std::uint8_t bytes[4]{
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 24U),
        };
        stream_.write(reinterpret_cast<const char*>(bytes), 4);
    }
    void put16(std::uint16_t value) {
        const std::uint8_t bytes[2]{
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8U),
        };
        stream_.write(reinterpret_cast<const char*>(bytes), 2);
    }

    void write_header() {
        const std::uint32_t block_align = channels_ * (bits_per_sample_ / 8U);
        stream_.write("RIFF", 4);
        put32(static_cast<std::uint32_t>(36 + data_bytes_));
        stream_.write("WAVE", 4);
        stream_.write("fmt ", 4);
        put32(16);
        put16(1); // PCM
        put16(channels_);
        put32(sample_rate_);
        put32(sample_rate_ * block_align);
        put16(static_cast<std::uint16_t>(block_align));
        put16(bits_per_sample_);
        stream_.write("data", 4);
        put32(static_cast<std::uint32_t>(data_bytes_));
    }

    std::ofstream stream_;
    std::size_t data_bytes_{};
    std::uint32_t sample_rate_{48000};
    std::uint16_t channels_{2};
    std::uint16_t bits_per_sample_{16};
};

// Writes the parameter sets once, then every sample as Annex-B so the file is
// decodable on its own. QuickTime carries AVCC block data plus the parameter
// sets in the format description, which a bare stream dump would lose.
class AnnexBWriter final {
public:
    bool open(const std::string& path) {
        stream_.open(path, std::ios::binary | std::ios::trunc);
        return stream_.is_open();
    }

    void write_parameter_sets(const coremedia::FormatDescription& format) {
        if (parameter_sets_written_) return;
        parameter_sets_written_ = true;
        for (const auto* group : {&format.video_parameter_sets,
                 &format.sequence_parameter_sets, &format.picture_parameter_sets}) {
            for (const auto& parameter_set : *group) append_start_code(parameter_set);
        }
    }

    void append_sample(std::span<const std::uint8_t> avcc,
        std::uint8_t length_field_bytes) {
        if (!stream_) return;
        const auto annex_b = h264::avcc_to_annex_b(avcc, length_field_bytes);
        stream_.write(reinterpret_cast<const char*>(annex_b.data()),
            static_cast<std::streamsize>(annex_b.size()));
        bytes_ += annex_b.size();
        ++samples_;
    }

    void close() { stream_.close(); }

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    [[nodiscard]] bool parameter_sets_written() const noexcept {
        return parameter_sets_written_;
    }

private:
    void append_start_code(const std::vector<std::uint8_t>& payload) {
        if (!stream_ || payload.empty()) return;
        static constexpr std::uint8_t start_code[4]{0, 0, 0, 1};
        stream_.write(reinterpret_cast<const char*>(start_code), 4);
        stream_.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
        bytes_ += 4 + payload.size();
    }

    std::ofstream stream_;
    std::size_t bytes_{};
    std::uint64_t samples_{};
    bool parameter_sets_written_{};
};

} // namespace

namespace {

// Samples the device state through sysfs and drives the recovery policy until
// it either authorizes a claim or gives up. sysfs is used rather than libusb
// because reading bConfigurationValue does not require opening the node, so it
// still works in the window where udev has not granted access yet.
struct RecoveryOutcome {
    bool ready{};
    std::uint32_t configuration_attempts{};
    std::uint32_t configuration_overwrites{};
    std::string diagnostic;
};

RecoveryOutcome recover_quicktime_configuration(device::UdevAppleMonitor& monitor,
    transport::QtUsbContext& context, const transport::AppleUsbIdentity& identity,
    const std::string& port_path, std::chrono::seconds timeout, bool verbose) {
    using capture::detail::classify_reenumeration_state;
    using capture::detail::ReenumerationAction;
    using capture::detail::UsbReenumerationPolicy;

    RecoveryOutcome outcome;
    UsbReenumerationPolicy policy;
    const auto deadline = Clock::now() + timeout;

    while (Clock::now() < deadline) {
        // The monitor's wait doubles as the settle interval: an event means the
        // state just changed, a timeout means it is quiet enough to re-sample.
        (void)monitor.wait_for_event(std::chrono::milliseconds(250));

        std::optional<device::UdevAppleDevice> sample;
        for (const auto& candidate : monitor.enumerate()) {
            // The port path is what survives re-enumeration; the serial may be
            // unreadable while the device is coming back up.
            const bool same_port = !port_path.empty() &&
                candidate.port_path == port_path;
            const bool same_serial = !identity.serial.empty() &&
                transport::apple_usb_serial_equal(candidate.serial, identity.serial);
            if (same_port || same_serial) {
                sample = candidate;
                break;
            }
        }

        bool quicktime_descriptor{};
        if (sample) {
            // The descriptor set is only visible through libusb; sysfs does not
            // expose interface classes of inactive configurations.
            try {
                if (const auto device = context.find_apple_device(identity, false))
                    quicktime_descriptor = device->quicktime_configuration;
            } catch (...) {
            }
            if (quicktime_descriptor) policy.note_quicktime_descriptor_present();
        }

        const auto observation = classify_reenumeration_state(
            sample.has_value(), quicktime_descriptor,
            sample && sample->active_configuration.has_value(),
            sample && sample->active_configuration
                ? *sample->active_configuration : std::uint8_t{0},
            identity.expected_quicktime_configuration);
        const auto action = policy.observe(observation);

        if (verbose) {
            std::fprintf(stderr,
                "recovery observation=%d action=%d active_config=%d attempts=%u overwrites=%u\n",
                static_cast<int>(observation), static_cast<int>(action),
                sample && sample->active_configuration
                    ? static_cast<int>(*sample->active_configuration) : -1,
                policy.configuration_attempts(), policy.configuration_overwrites());
        }

        if (action == ReenumerationAction::SetQuickTimeConfiguration) {
            const auto applied = sample
                ? transport::set_active_configuration(context, sample->bus,
                      sample->address, identity.expected_quicktime_configuration)
                : transport::SetConfigurationResult{
                      .applied = false,
                      .diagnostic = "no sysfs sample for this device",
                  };
            logging::write(std::format(
                "wp4_recovery set_configuration value={} attempt={} applied={} diagnostic={}",
                identity.expected_quicktime_configuration,
                policy.configuration_attempts(), applied.applied,
                applied.diagnostic));
            continue;
        }
        if (action == ReenumerationAction::Claim) {
            outcome.ready = true;
            break;
        }
        if (action == ReenumerationAction::GiveUp) {
            outcome.diagnostic =
                "the QuickTime configuration could not be made to stick; "
                "re-plug the cable to reset the device";
            break;
        }
    }

    outcome.configuration_attempts = policy.configuration_attempts();
    outcome.configuration_overwrites = policy.configuration_overwrites();
    if (!outcome.ready && outcome.diagnostic.empty()) {
        outcome.diagnostic = "timed out waiting for the QuickTime configuration";
    }
    return outcome;
}

// Opens the QuickTime endpoint, runs the upstream handshake state machine and
// writes every sample it yields to disk. The protocol side is the shared
// SessionProtocol, so a success here also proves the handshake works on Linux.
int stream_to_disk(transport::QtUsbContext& context,
    const transport::AppleUsbIdentity& identity, const Options& options) {
    std::error_code directory_error;
    std::filesystem::create_directories(
        std::filesystem::path(options.video_path).parent_path(), directory_error);

    AnnexBWriter video;
    if (!video.open(options.video_path)) {
        std::fprintf(stderr, "cannot write %s\n", options.video_path.c_str());
        return 1;
    }
    WavWriter audio;

    transport::QtUsbConnection connection;
    try {
        connection = transport::QtUsbConnection::open_quicktime(context, identity,
            false);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cannot open the QuickTime endpoint: %s\n",
            error.what());
        return 1;
    }
    try { connection.clear_halt(); } catch (...) {}

    quicktime::SessionOptions session_options;
    session_options.demo_mode = true;
    session_options.request_audio = true;
    quicktime::SessionProtocol protocol(session_options);
    quicktime::StreamDecoder decoder;

    std::vector<std::uint8_t> read_buffer(1024U * 1024U);
    std::uint64_t audio_packets{};
    std::uint8_t nalu_length_size{4};
    const auto deadline = Clock::now() + options.duration;
    bool ping_sent{};

    while (Clock::now() < deadline) {
        std::size_t count{};
        try {
            count = connection.read(read_buffer, 250);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "bulk read failed: %s\n", error.what());
            break;
        }
        if (count == 0) {
            // The reference flow sends one PING after the first read timeout so
            // a freshly activated endpoint starts talking.
            if (!ping_sent) {
                ping_sent = true;
                try {
                    connection.recover_handshake();
                    connection.write(quicktime::make_ping(), 1000);
                } catch (...) {}
            }
            continue;
        }

        for (const auto& packet : decoder.push(std::span(read_buffer).first(count))) {
            quicktime::SessionEvent event;
            try {
                event = protocol.process(packet);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "protocol error: %s\n", error.what());
                goto finished;
            }
            for (const auto& response : event.outbound) {
                try { connection.write(response, 1000); } catch (...) {}
            }
            if (event.video_sample) {
                const auto& sample = *event.video_sample;
                const auto* format = sample.format ? &*sample.format
                    : protocol.video_format() ? &*protocol.video_format() : nullptr;
                if (format != nullptr) {
                    if (format->nalu_length_size >= 1 && format->nalu_length_size <= 4)
                        nalu_length_size = format->nalu_length_size;
                    video.write_parameter_sets(*format);
                }
                video.append_sample(sample.sample_data, nalu_length_size);
            }
            if (event.audio_sample) {
                const auto& sample = *event.audio_sample;
                const auto* format = sample.format && sample.format->audio
                    ? &*sample.format
                    : protocol.audio_format() && protocol.audio_format()->audio
                        ? &*protocol.audio_format() : nullptr;
                if (format != nullptr && !audio.is_open()) {
                    const auto& description = *format->audio;
                    if (!audio.open(options.audio_path,
                            static_cast<std::uint32_t>(description.sample_rate),
                            static_cast<std::uint16_t>(description.channels_per_frame),
                            static_cast<std::uint16_t>(description.bits_per_channel))) {
                        std::fprintf(stderr, "cannot write %s\n",
                            options.audio_path.c_str());
                    }
                }
                audio.append(sample.sample_data);
                ++audio_packets;
            }
        }
    }

finished:
    // Every exit path must send the same HPA0/HPD0 stop controls the working
    // clients send, then restore the normal configuration.
    for (const auto& message : protocol.stop_messages()) {
        try { connection.write(message, 500); } catch (...) {}
    }
    const bool normal_requested = [&] {
        try { return connection.request_normal_configuration(); }
        catch (...) { return false; }
    }();
    connection.close();

    video.close();
    audio.close();

    std::printf("video                 : %s samples=%llu bytes=%zu parameter_sets=%s\n",
        options.video_path.c_str(),
        static_cast<unsigned long long>(video.samples()), video.bytes(),
        video.parameter_sets_written() ? "yes" : "no");
    std::printf("audio                 : %s packets=%llu bytes=%zu\n",
        options.audio_path.c_str(),
        static_cast<unsigned long long>(audio_packets), audio.data_bytes());
    std::printf("video frames (protocol): %llu\n",
        static_cast<unsigned long long>(protocol.video_frames()));
    std::printf("normal configuration  : %s\n",
        normal_requested ? "requested" : "not acknowledged");

    if (video.samples() == 0) {
        std::fprintf(stderr,
            "no video sample arrived; keep the device unlocked and retry\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse(argc, argv);
    if (!options) {
        print_usage();
        return 64;
    }
    logging::initialize();

    device::UdevAppleMonitor monitor;
    if (!monitor.valid()) {
        std::fprintf(stderr, "cannot open the udev netlink monitor\n");
        return 1;
    }

    transport::AppleUsbIdentity identity;
    identity.serial = options->serial;
    std::string port_path;

    try {
        transport::QtUsbContext preflight(false);
        const auto device = preflight.find_apple_device(identity, false);
        if (!device) {
            std::fprintf(stderr,
                "no Apple device with serial %s; check the udev rule and usbmuxd\n",
                options->serial.c_str());
            return 1;
        }
        identity = transport::make_apple_usb_identity(*device);
        for (const auto& candidate : monitor.enumerate()) {
            if (transport::apple_usb_serial_equal(candidate.serial, identity.serial)) {
                port_path = candidate.port_path;
                break;
            }
        }
        std::printf("device                : serial=%s pid=%04x port=%s\n",
            identity.serial.c_str(), device->product_id, port_path.c_str());
        std::printf("configurations        : count=%u highest=%u expected_qt=%u\n",
            device->configuration_count, device->highest_configuration_value,
            identity.expected_quicktime_configuration);
        std::printf("quicktime descriptor  : %s\n",
            device->quicktime_configuration ? "present" : "absent");

        if (!device->quicktime_configuration) {
            // 0x52 with wIndex=2 asks iOS to expose the hidden capture
            // configuration. The device detaches; udev then re-adds it.
            std::printf("enabling the QuickTime configuration (0x52 wIndex=2)\n");
            const bool acknowledged =
                transport::QtUsbConnection::enable_quicktime_configuration(
                    preflight, identity);
            std::printf("0x52 acknowledged     : %s\n",
                acknowledged ? "yes" : "no (re-enumeration is the authority)");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "preflight failed: %s\n", error.what());
        return 1;
    }

    // A fresh context: the previous one's device list is stale after the detach.
    transport::QtUsbContext context(false);
    const auto recovery = recover_quicktime_configuration(monitor, context,
        identity, port_path, std::chrono::seconds(30), options->verbose);
    std::printf("recovery              : ready=%s attempts=%u overwrites=%u %s\n",
        recovery.ready ? "yes" : "no", recovery.configuration_attempts,
        recovery.configuration_overwrites, recovery.diagnostic.c_str());
    if (recovery.configuration_overwrites > 0) {
        std::printf("  udev wrote bConfigurationValue back to 0 %u time(s): "
                    "39-usbmuxd.rules is active on this device\n",
            recovery.configuration_overwrites);
    }
    if (!recovery.ready) return 1;

    return stream_to_disk(context, identity, *options);
}

