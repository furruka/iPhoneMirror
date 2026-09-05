// SPDX-License-Identifier: GPL-3.0-only
//
// WP5 acceptance tool for the Linux audio renderer. Needs no device: it builds
// the same interleaved signed PCM16 format the QuickTime capture path negotiates,
// pushes a generated tone through the real IAudioRenderer seam in packets the
// size the device sends, and reports the playback counters.
//
// The evidence is rendered_frames advancing while dropped and underrun counts
// stay near zero. That can only happen if the PipeWire graph is pulling from this
// renderer's ring, so it covers the connection, the format negotiation and the
// process callback at once.

#include "Audio/LinuxPipeWireAudioRenderer.h"
#include "Audio/PcmBufferPolicy.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

using namespace iPhoneMirror;

namespace {

constexpr double SampleRate = 44100.0;
constexpr std::uint32_t Channels = 2;
constexpr std::size_t PacketFrames = 512;

[[nodiscard]] coremedia::AudioStreamBasicDescription quicktime_pcm16() noexcept {
    coremedia::AudioStreamBasicDescription format;
    format.sample_rate = SampleRate;
    format.format_id = audio::detail::LinearPcm;
    format.format_flags = audio::detail::PcmIsSignedInteger;
    format.channels_per_frame = Channels;
    format.bits_per_channel = 16;
    format.bytes_per_frame = Channels * 2U;
    format.frames_per_packet = 1;
    format.bytes_per_packet = format.bytes_per_frame;
    return format;
}

void report(const char* label, const audio::PlaybackStats& stats) {
    std::printf("%-22s: active=%s queued=%llu rendered=%llu dropped=%llu "
                "underruns=%llu\n",
        label, stats.active ? "yes" : "no",
        static_cast<unsigned long long>(stats.queued_frames),
        static_cast<unsigned long long>(stats.rendered_frames),
        static_cast<unsigned long long>(stats.dropped_frames),
        static_cast<unsigned long long>(stats.underruns));
}

} // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 3;
    if (seconds <= 0) {
        std::fprintf(stderr, "usage: iPhoneMirror.Linux.AudioProbe [seconds]\n");
        return 2;
    }

    const auto format = quicktime_pcm16();
    std::unique_ptr<audio::IAudioRenderer> renderer;
    try {
        renderer = audio::make_pipewire_audio_renderer(format, true, 0.2F);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "renderer construction failed: %s\n", error.what());
        return 1;
    }
    std::printf("format                : %.0f Hz %u ch s16le packet=%zu frames\n",
        SampleRate, Channels, PacketFrames);

    // A 440 Hz tone at the requested volume. Loud enough to hear, quiet enough
    // not to startle whoever runs this.
    std::vector<std::uint8_t> packet(PacketFrames * format.bytes_per_frame);
    auto* samples = reinterpret_cast<std::int16_t*>(packet.data());
    std::uint64_t phase_frame{};
    const auto packet_period = std::chrono::duration<double>(
        static_cast<double>(PacketFrames) / SampleRate);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    auto next_packet = started;
    std::uint64_t enqueued_frames{};
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::size_t frame{}; frame < PacketFrames; ++frame) {
            const auto angle = 2.0 * std::numbers::pi * 440.0 *
                static_cast<double>(phase_frame + frame) / SampleRate;
            const auto value = static_cast<std::int16_t>(
                std::lround(std::sin(angle) * 24000.0));
            for (std::uint32_t channel{}; channel < Channels; ++channel)
                samples[frame * Channels + channel] = value;
        }
        phase_frame += PacketFrames;
        renderer->enqueue(packet);
        enqueued_frames += PacketFrames;
        next_packet += std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(packet_period);
        std::this_thread::sleep_until(next_packet);
    }

    const auto stats = renderer->stats();
    report("while streaming", stats);
    renderer->stop();
    std::printf("enqueued frames       : %llu (%d s at %.0f Hz)\n",
        static_cast<unsigned long long>(enqueued_frames), seconds, SampleRate);

    if (!stats.active) {
        std::fprintf(stderr,
            "the stream never reached PW_STREAM_STATE_STREAMING\n");
        return 1;
    }
    // Half of what was pushed is a generous floor: it only fails if the graph
    // was barely pulling, which is the failure this tool exists to catch.
    if (stats.rendered_frames * 2 < enqueued_frames) {
        std::fprintf(stderr, "the graph rendered %llu of %llu enqueued frames\n",
            static_cast<unsigned long long>(stats.rendered_frames),
            static_cast<unsigned long long>(enqueued_frames));
        return 1;
    }
    std::printf("verdict               : PASS\n");
    return 0;
}
