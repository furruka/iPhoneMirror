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
#include <utility>
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
    // libusb_clear_halt sends CLEAR_FEATURE(ENDPOINT_HALT), which resets the
    // host-side data toggle. On a freshly armed endpoint that can desynchronize
    // the pipe from the device's own toggle, and the observable result is exactly
    // what this tool measured: bulk IN delivers the device's first packet while
    // every bulk OUT write times out. The Windows path clears the halt, so this
    // switch exists to test whether Linux should.
    bool clear_halt{true};
};

void print_usage() {
    std::fprintf(stderr,
        "usage: iPhoneMirror.Linux.HeadlessCapture --serial <udid> "
        "[--video <path.h264>] [--audio <path.wav>] [--seconds N] "
        "[--no-clear-halt] [--verbose]\n");
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
        } else if (argument == "--no-clear-halt") {
            options.clear_halt = false;
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
    std::uint32_t claim_attempts{};
    std::string diagnostic;
    // Claimed here rather than by the caller: usbmuxd re-selects a
    // configuration of its own within a few hundred milliseconds, and a claimed
    // interface is what stops it, so the claim cannot wait for the next loop.
    transport::ClaimedQuickTimeInterface connection;
};

// Sets the configuration and claims in one call. Splitting the two lost the race
// to usbmuxd by 14 ms; see Transport/LinuxUsbConfiguration.h.
bool try_claim(transport::QtUsbContext& context, std::uint8_t bus,
    std::uint8_t address, std::uint8_t configuration, RecoveryOutcome& outcome) {
    ++outcome.claim_attempts;
    std::string diagnostic;
    auto claimed = transport::ClaimedQuickTimeInterface::open(context, bus, address,
        configuration, diagnostic);
    if (claimed.valid()) {
        logging::write(std::format(
            "wp4_recovery claimed attempt={} configuration_was_set={} in=0x{:02x} out=0x{:02x}",
            outcome.claim_attempts, claimed.configuration_was_set(),
            claimed.endpoints().bulk_in, claimed.endpoints().bulk_out));
        outcome.connection = std::move(claimed);
        outcome.ready = true;
        return true;
    }
    logging::write(std::format("wp4_recovery claim_failed attempt={} error={}",
        outcome.claim_attempts, diagnostic));
    return false;
}

RecoveryOutcome recover_quicktime_configuration(device::UdevAppleMonitor& monitor,
    transport::QtUsbContext& context, const transport::AppleUsbIdentity& identity,
    const std::string& port_path, std::chrono::seconds timeout,
    bool expect_detach, bool verbose) {
    using capture::detail::classify_reenumeration_state;
    using capture::detail::ReenumerationAction;
    using capture::detail::UsbReenumerationPolicy;

    RecoveryOutcome outcome;
    UsbReenumerationPolicy policy;
    const auto deadline = Clock::now() + timeout;
    // The 0x52 request takes the better part of a second to detach the device.
    // Driving the policy before that happens spends the whole attempt budget on
    // the pre-detach instance, whose configuration usbmuxd is still holding, so
    // every request comes back LIBUSB_ERROR_BUSY and the recovery gives up
    // before the window it was waiting for ever opens.
    bool detach_seen = !expect_detach;

    while (Clock::now() < deadline) {
        // Short wait: an event means the state just changed and the window
        // before usbmuxd claims its own interface is only tens of milliseconds.
        (void)monitor.wait_for_event(std::chrono::milliseconds(50));

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

        // Deliberately not find_apple_device: that opens every Apple device to
        // read serials and costs more than the whole race window. The
        // configuration count from sysfs answers the same question, because the
        // QuickTime configuration is the one 0x52 appends.
        const bool quicktime_descriptor = sample &&
            identity.expected_quicktime_configuration != 0 &&
            sample->configuration_count >= identity.expected_quicktime_configuration;
        if (quicktime_descriptor) policy.note_quicktime_descriptor_present();

        if (!sample) detach_seen = true;
        if (!detach_seen) {
            if (verbose) {
                std::fprintf(stderr,
                    "recovery waiting for the 0x52 detach (still enumerated)\n");
            }
            continue;
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
            // One call: setting the configuration and then re-finding the device
            // to claim it leaves a gap usbmuxd wins.
            if (sample && try_claim(context, sample->bus, sample->address,
                    identity.expected_quicktime_configuration, outcome)) break;
            continue;
        }
        if (action == ReenumerationAction::Claim) {
            if (sample && try_claim(context, sample->bus, sample->address,
                    identity.expected_quicktime_configuration, outcome)) break;
            // Another process holds the interface. Keep sampling until the
            // deadline rather than declaring failure on one lost race.
            continue;
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
int stream_to_disk(const Options& options,
    transport::ClaimedQuickTimeInterface connection) {
    std::error_code directory_error;
    std::filesystem::create_directories(
        std::filesystem::path(options.video_path).parent_path(), directory_error);

    AnnexBWriter video;
    if (!video.open(options.video_path)) {
        std::fprintf(stderr, "cannot write %s\n", options.video_path.c_str());
        return 1;
    }
    WavWriter audio;

    {
        const auto& endpoints = connection.endpoints();
        std::printf("endpoints             : config=%u interface=%u alt=%u "
                    "in=0x%02x out=0x%02x in_packet=%u out_packet=%u\n",
            endpoints.configuration, endpoints.interface_number,
            endpoints.alternate_setting, endpoints.bulk_in, endpoints.bulk_out,
            endpoints.bulk_in_packet_size, endpoints.bulk_out_packet_size);
        std::printf("configuration_was_set : %s\n",
            connection.configuration_was_set() ? "yes" : "no (already active)");
    }

    // clear_halt is deliberately not issued: CLEAR_FEATURE(ENDPOINT_HALT) resets
    // the host-side data toggle, and on a freshly armed endpoint that can leave
    // the OUT pipe out of step with the device. The Windows path needs it for the
    // libusb-win32 backend; nothing here does.
    std::printf("clear_halt            : not issued on Linux\n");

    quicktime::SessionOptions session_options;
    session_options.demo_mode = true;
    session_options.request_audio = true;
    quicktime::SessionProtocol protocol(session_options);
    quicktime::StreamDecoder decoder;

    std::vector<std::uint8_t> read_buffer(1024U * 1024U);
    std::uint64_t audio_packets{};
    std::uint8_t nalu_length_size{4};
    std::uint64_t bytes_read{};
    std::uint64_t reads_with_data{};
    std::uint64_t packets_decoded{};
    auto last_state = quicktime::SessionState::WaitingForPing;
    const auto deadline = Clock::now() + options.duration;
    auto next_ping = Clock::now() + std::chrono::seconds(3);
    std::uint64_t ping_attempts{};
    std::uint64_t outbound_written{};
    std::uint64_t outbound_failed{};
    bool ping_sent{};

    while (Clock::now() < deadline) {
        std::string io_diagnostic;
        const auto count = connection.read(read_buffer, 250, io_diagnostic);
        if (!io_diagnostic.empty()) {
            std::fprintf(stderr, "bulk read failed: %s\n", io_diagnostic.c_str());
            break;
        }
        if (count != 0) {
            bytes_read += count;
            ++reads_with_data;
        }
        if (count == 0) {
            // The reference flow sends one PING after the first read timeout so
            // a freshly activated endpoint starts talking. Retried here on a
            // slow cadence, and every step reported: a silent device is the
            // failure being diagnosed, so a swallowed write error would hide the
            // answer.
            const auto now = Clock::now();
            // The reference protocol documentation is explicit that the device
            // speaks first: "we need to wait for the device to send us a ping
            // packet", and only then does the host reply. So do not open with a
            // PING. The control kick plus an unsolicited PING stays as a late
            // fallback for an endpoint that never starts on its own, which is
            // what the Windows path also does after its first read timeout.
            if (now >= next_ping) {
                next_ping = now + std::chrono::seconds(2);
                ++ping_attempts;
                if (!ping_sent) {
                    ping_sent = true;
                    std::string kick_diagnostic;
                    const bool kicked = connection.kick_handshake(kick_diagnostic);
                    std::printf("handshake kick        : %s\n",
                        kicked ? "ok" : kick_diagnostic.c_str());
                }
                std::string write_diagnostic;
                if (connection.write(quicktime::make_ping(), 1000,
                        write_diagnostic)) {
                    if (ping_attempts <= 2) {
                        std::printf("ping write            : ok (attempt %llu)\n",
                            static_cast<unsigned long long>(ping_attempts));
                    }
                } else if (ping_attempts <= 3) {
                    std::printf("ping write            : failed (%s)\n",
                        write_diagnostic.c_str());
                }
            }
            continue;
        }

        for (const auto& packet : decoder.push(std::span(read_buffer).first(count))) {
            ++packets_decoded;
            quicktime::SessionEvent event;
            try {
                event = protocol.process(packet);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "protocol error: %s\n", error.what());
                goto finished;
            }
            for (const auto& response : event.outbound) {
                // Reported rather than swallowed: the device answering while the
                // host cannot answer back is the exact failure being diagnosed.
                std::string write_diagnostic;
                if (connection.write(response, 1000, write_diagnostic)) {
                    ++outbound_written;
                } else {
                    ++outbound_failed;
                    if (outbound_failed <= 3) {
                        std::printf("outbound write        : failed (%zu bytes, %s)\n",
                            response.size(), write_diagnostic.c_str());
                    }
                }
            }
            if (event.state != last_state) {
                last_state = event.state;
                std::printf("handshake state       : %d\n",
                    static_cast<int>(last_state));
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
        std::string ignored;
        (void)connection.write(message, 500, ignored);
    }
    const bool normal_requested = connection.request_normal_configuration();
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
    std::printf("outbound writes       : ok=%llu failed=%llu\n",
        static_cast<unsigned long long>(outbound_written),
        static_cast<unsigned long long>(outbound_failed));
    std::printf("ping attempts         : %llu\n",
        static_cast<unsigned long long>(ping_attempts));
    std::printf("bulk reads            : with_data=%llu bytes=%llu packets=%llu\n",
        static_cast<unsigned long long>(reads_with_data),
        static_cast<unsigned long long>(bytes_read),
        static_cast<unsigned long long>(packets_decoded));
    std::printf("handshake final state : %d (0=WaitingForPing)\n",
        static_cast<int>(protocol.state()));
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
    bool detach_expected{};

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
        const bool quicktime_active = device->active_configuration_known &&
            identity.expected_quicktime_configuration != 0 &&
            device->active_configuration ==
                identity.expected_quicktime_configuration;
        std::printf("quicktime descriptor  : %s%s\n",
            device->quicktime_configuration ? "present" : "absent",
            quicktime_active ? " (and active)" : "");

        // iOS arms the Valeria endpoints only for a short window after the
        // configuration switch, and 0x52 wIndex=2 is a no-op once the
        // configuration is already exposed. So whenever the extra configuration
        // exists this run has to remove it and put it back, including when it is
        // the active one: claiming an already-active configuration arrives long
        // after its window closed. Measured on both an iPad Air M3 and an
        // iPhone 16 Pro: the extra configuration survives unplugging the cable,
        // and 0x52 wIndex=0 is acknowledged immediately but the configuration
        // only disappears around a minute later. That request is the reset, and
        // it has to be waited out rather than replaced by a replug.
        if (device->quicktime_configuration) {
            std::printf("disabling the QuickTime configuration first "
                        "(0x52 wIndex=0) so a fresh window can open\n");
            bool disable_acknowledged{};
            try {
                disable_acknowledged =
                    transport::QtUsbConnection::disable_quicktime_configuration(
                        preflight, identity);
            } catch (const std::exception& error) {
                std::printf("0x52 wIndex=0         : threw (%s)\n", error.what());
            }
            std::printf("0x52 wIndex=0         : %s\n",
                disable_acknowledged ? "acknowledged" : "not acknowledged");
            // Wait for the device to come back without the extra configuration.
            const auto reset_started = Clock::now();
            const auto reset_deadline = reset_started + std::chrono::seconds(180);
            bool reset_seen{};
            while (Clock::now() < reset_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                bool present{};
                for (const auto& candidate : monitor.enumerate()) {
                    if (!transport::apple_usb_serial_equal(candidate.serial,
                            identity.serial)) continue;
                    present = true;
                    if (candidate.configuration_count <
                        identity.expected_quicktime_configuration) {
                        reset_seen = true;
                    }
                }
                if (reset_seen && present) break;
            }
            std::printf("configuration reset   : %s (waited %llds)\n",
                reset_seen ? "yes" : "no (the extra configuration is still there)",
                static_cast<long long>(std::chrono::duration_cast<
                    std::chrono::seconds>(Clock::now() - reset_started).count()));
        }

        // 0x52 with wIndex=2 asks iOS to expose the hidden capture configuration
        // and detaches the device. That detach is also the only moment
        // SET_CONFIGURATION can succeed, because once usbmuxd has claimed an
        // interface the request returns LIBUSB_ERROR_BUSY.
        std::printf("forcing re-enumeration (0x52 wIndex=2) to open the "
                    "configuration window\n");
        const bool acknowledged =
            transport::QtUsbConnection::enable_quicktime_configuration(
                preflight, identity);
        std::printf("0x52 acknowledged     : %s\n",
            acknowledged ? "yes" : "no (re-enumeration is the authority)");
        detach_expected = true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "preflight failed: %s\n", error.what());
        return 1;
    }

    // A fresh context: the previous one's device list is stale after the detach.
    transport::QtUsbContext context(false);
    auto recovery = recover_quicktime_configuration(monitor, context,
        identity, port_path, std::chrono::seconds(30), detach_expected,
        options->verbose);
    std::printf("recovery              : ready=%s set_attempts=%u overwrites=%u "
                "claim_attempts=%u %s\n",
        recovery.ready ? "yes" : "no", recovery.configuration_attempts,
        recovery.configuration_overwrites, recovery.claim_attempts,
        recovery.diagnostic.c_str());
    if (recovery.configuration_overwrites > 0) {
        std::printf("  udev wrote bConfigurationValue back to 0 %u time(s): "
                    "39-usbmuxd.rules is active on this device\n",
            recovery.configuration_overwrites);
    }
    if (!recovery.ready || !recovery.connection.valid()) return 1;

    return stream_to_disk(*options, std::move(recovery.connection));
}

