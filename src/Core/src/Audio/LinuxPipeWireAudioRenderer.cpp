// SPDX-License-Identifier: GPL-3.0-only
//
// Linux audio renderer built on PipeWire, behind the same IAudioRenderer seam as
// the Windows WASAPI renderer.
//
// The queue decisions are not reimplemented here: capacity, startup and
// high-water thresholds and the drop plan all come from Audio/PcmBufferPolicy.h,
// which was extracted from WasapiRenderer precisely so both platforms make the
// same choices about a late or oversized packet. What is local to this file is
// the ring's memcpy mechanics and the PipeWire plumbing.
//
// Gain is applied in software in the same 1/10000 units as the Windows renderer,
// ramped across one PipeWire buffer, so a volume change or a mute toggle sounds
// the same on both platforms instead of clicking. Routing volume through the
// session manager instead would make playback depend on which manager is
// installed.

#include "Audio/LinuxPipeWireAudioRenderer.h"

#include "Audio/PcmBufferPolicy.h"
#include "Logging.h"

extern "C" {
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace iPhoneMirror::audio {

namespace {

// pw_init is process-wide and must not run twice.
void ensure_pipewire_initialized() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

class PipeWireAudioRenderer final : public IAudioRenderer {
public:
    PipeWireAudioRenderer(const coremedia::AudioStreamBasicDescription& format,
        bool playback_enabled, float volume)
        : format_(format) {
        const auto layout = detail::checked_wasapi_buffer_layout(format);
        if (!layout)
            throw std::invalid_argument("unsupported QuickTime audio format");
        block_align_ = layout->block_align;
        capacity_frames_ = layout->capacity_frames;
        ring_.resize(layout->capacity_bytes);
        playback_enabled_.store(playback_enabled, std::memory_order_relaxed);
        set_volume(volume);
        current_gain_units_ = playback_enabled ? volume_units_.load(
            std::memory_order_relaxed) : 0U;

        ensure_pipewire_initialized();
        loop_ = pw_thread_loop_new("iPhoneMirror audio", nullptr);
        if (loop_ == nullptr)
            throw std::runtime_error("pw_thread_loop_new failed");
        // The destructor does not run for a constructor that threw, so the loop
        // has to be released here or it leaks together with its thread.
        try {
            connect();
        } catch (...) {
            teardown();
            throw;
        }
    }

    ~PipeWireAudioRenderer() override { teardown(); }

    PipeWireAudioRenderer(const PipeWireAudioRenderer&) = delete;
    PipeWireAudioRenderer& operator=(const PipeWireAudioRenderer&) = delete;

    void enqueue(std::span<const std::uint8_t> pcm) override {
        if (pcm.empty() || block_align_ == 0) return;
        if (pcm.size() % block_align_ != 0) {
            logging::write(std::format(
                "pipewire drop malformed_bytes={} block_align={}", pcm.size(),
                block_align_));
            return;
        }
        auto frames = pcm.size() / block_align_;
        if (frames > capacity_frames_) {
            const auto trimmed = frames - capacity_frames_;
            pcm = pcm.subspan(trimmed * block_align_);
            frames = capacity_frames_;
            dropped_frames_.fetch_add(trimmed, std::memory_order_relaxed);
        }

        std::size_t dropped{};
        {
            std::scoped_lock lock(queue_mutex_);
            recent_packet_frames_[packet_history_index_] = frames;
            packet_history_index_ =
                (packet_history_index_ + 1) % recent_packet_frames_.size();
            const auto maximum_packet = *std::max_element(
                recent_packet_frames_.begin(), recent_packet_frames_.end());
            const auto thresholds = detail::wasapi_queue_thresholds(
                maximum_packet, capacity_frames_, quantum_frames_.load(
                    std::memory_order_relaxed));
            const auto plan = detail::plan_wasapi_enqueue(queued_frames_, frames,
                capacity_frames_, thresholds);
            dropped = plan.drop_existing_frames;
            if (dropped != 0) {
                read_frame_ = (read_frame_ + dropped) % capacity_frames_;
                queued_frames_ -= dropped;
            }
            const auto first = std::min(frames, capacity_frames_ - write_frame_);
            std::memcpy(ring_.data() + write_frame_ * block_align_, pcm.data(),
                first * block_align_);
            if (frames > first) {
                std::memcpy(ring_.data(), pcm.data() + first * block_align_,
                    (frames - first) * block_align_);
            }
            write_frame_ = (write_frame_ + frames) % capacity_frames_;
            queued_frames_ += frames;
        }
        if (dropped != 0) {
            dropped_frames_.fetch_add(dropped, std::memory_order_relaxed);
            logging::write(std::format(
                "pipewire drop stale_frames={} incoming_frames={}", dropped,
                frames));
        }
    }

    void set_enabled(bool enabled) noexcept override {
        playback_enabled_.store(enabled, std::memory_order_relaxed);
    }

    void set_volume(float volume) noexcept override {
        if (!std::isfinite(volume)) return;
        const auto clamped = std::clamp(volume, 0.0F, 1.0F);
        volume_units_.store(static_cast<std::uint32_t>(
            std::lround(clamped * 10000.0F)), std::memory_order_relaxed);
    }

    void stop() noexcept override { teardown(); }

    [[nodiscard]] PlaybackStats stats() const override {
        std::size_t queued{};
        {
            std::scoped_lock lock(queue_mutex_);
            queued = queued_frames_;
        }
        return PlaybackStats{
            .active = active_.load(std::memory_order_relaxed),
            .queued_frames = queued,
            .rendered_frames = rendered_frames_.load(std::memory_order_relaxed),
            .dropped_frames = dropped_frames_.load(std::memory_order_relaxed),
            .underruns = underruns_.load(std::memory_order_relaxed),
        };
    }

private:
    void connect() {
        pw_thread_loop_lock(loop_);
        // Unlock and tear down on every failure path: pw_thread_loop_destroy
        // cannot run with the loop locked.
        struct LockGuard {
            pw_thread_loop* loop;
            ~LockGuard() { pw_thread_loop_unlock(loop); }
        } guard{loop_};

        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Movie",
            PW_KEY_APP_NAME, "iPhoneMirror",
            PW_KEY_NODE_NAME, "iPhoneMirror",
            nullptr);
        if (properties == nullptr)
            throw std::runtime_error("pw_properties_new failed");
        // pw_stream_new_simple consumes the properties even when it fails.
        stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
            "iPhoneMirror", properties, &stream_events, this);
        if (stream_ == nullptr)
            throw std::runtime_error("pw_stream_new_simple failed");

        std::uint8_t storage[1024];
        spa_pod_builder builder{};
        builder.data = storage;
        builder.size = sizeof storage;
        spa_audio_info_raw info{};
        // checked_wasapi_buffer_layout already established interleaved signed
        // PCM16, so there is exactly one format to offer.
        info.format = SPA_AUDIO_FORMAT_S16_LE;
        info.rate = static_cast<std::uint32_t>(format_.sample_rate);
        info.channels = format_.channels_per_frame;
        const spa_pod* params[1]{
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
        if (params[0] == nullptr)
            throw std::runtime_error("spa_format_audio_raw_build failed");

        const int result = pw_stream_connect(stream_, PW_DIRECTION_OUTPUT,
            PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (result < 0) {
            throw std::runtime_error(std::format("pw_stream_connect failed: {}",
                spa_strerror(result)));
        }
        if (const int started = pw_thread_loop_start(loop_); started < 0) {
            throw std::runtime_error(std::format("pw_thread_loop_start failed: {}",
                spa_strerror(started)));
        }
    }

    void teardown() noexcept {
        if (loop_ == nullptr) return;
        pw_thread_loop_stop(loop_);
        if (stream_ != nullptr) {
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        active_.store(false, std::memory_order_relaxed);
    }

    // Copies frames out of the ring and applies the gain ramp, mirroring the
    // Windows renderer's dequeue so both platforms sound the same.
    [[nodiscard]] std::size_t dequeue(std::uint8_t* destination,
        std::size_t frames) {
        std::scoped_lock lock(queue_mutex_);
        const auto count = std::min(frames, queued_frames_);
        if (count == 0) return 0;
        const auto first = std::min(count, capacity_frames_ - read_frame_);
        std::memcpy(destination, ring_.data() + read_frame_ * block_align_,
            first * block_align_);
        if (count > first) {
            std::memcpy(destination + first * block_align_, ring_.data(),
                (count - first) * block_align_);
        }
        read_frame_ = (read_frame_ + count) % capacity_frames_;
        queued_frames_ -= count;

        const auto target_gain = playback_enabled_.load(std::memory_order_relaxed)
            ? volume_units_.load(std::memory_order_relaxed)
            : 0U;
        const auto start_gain = current_gain_units_;
        auto* samples = reinterpret_cast<std::int16_t*>(destination);
        const auto channels = static_cast<std::size_t>(format_.channels_per_frame);
        const auto delta = static_cast<std::int64_t>(target_gain) - start_gain;
        for (std::size_t frame{}; frame < count; ++frame) {
            const auto gain = static_cast<std::int64_t>(start_gain) +
                delta * static_cast<std::int64_t>(frame + 1U) /
                    static_cast<std::int64_t>(count);
            for (std::size_t channel{}; channel < channels; ++channel) {
                const auto index = frame * channels + channel;
                const auto scaled =
                    static_cast<std::int32_t>(samples[index]) * gain / 10000;
                samples[index] = static_cast<std::int16_t>(scaled);
            }
        }
        current_gain_units_ = target_gain;
        return count;
    }

    void process() {
        pw_buffer* buffer = pw_stream_dequeue_buffer(stream_);
        if (buffer == nullptr) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        spa_data& data = buffer->buffer->datas[0];
        auto* destination = static_cast<std::uint8_t*>(data.data);
        if (destination == nullptr) {
            pw_stream_queue_buffer(stream_, buffer);
            return;
        }
        auto capacity = static_cast<std::size_t>(data.maxsize) / block_align_;
        if (buffer->requested != 0) {
            capacity = std::min(capacity,
                static_cast<std::size_t>(buffer->requested));
            quantum_frames_.store(static_cast<std::size_t>(buffer->requested),
                std::memory_order_relaxed);
        }

        const auto rendered = dequeue(destination, capacity);
        if (rendered < capacity) {
            // Silence rather than a short buffer: the graph expects the whole
            // quantum, and a short one is heard as a click.
            std::memset(destination + rendered * block_align_, 0,
                (capacity - rendered) * block_align_);
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        rendered_frames_.fetch_add(rendered, std::memory_order_relaxed);

        data.chunk->offset = 0;
        data.chunk->stride = static_cast<std::int32_t>(block_align_);
        data.chunk->size = static_cast<std::uint32_t>(capacity * block_align_);
        pw_stream_queue_buffer(stream_, buffer);
    }

    static void on_process(void* userdata) {
        static_cast<PipeWireAudioRenderer*>(userdata)->process();
    }

    static void on_state_changed(void* userdata, enum pw_stream_state,
        enum pw_stream_state state, const char* error) {
        auto* self = static_cast<PipeWireAudioRenderer*>(userdata);
        self->active_.store(state == PW_STREAM_STATE_STREAMING,
            std::memory_order_relaxed);
        if (state == PW_STREAM_STATE_ERROR) {
            logging::write(std::format("pipewire stream error={}",
                error != nullptr ? error : "(none)"));
        }
    }

    static const pw_stream_events stream_events;

    coremedia::AudioStreamBasicDescription format_;
    std::uint32_t block_align_{};
    std::size_t capacity_frames_{};
    std::vector<std::uint8_t> ring_;

    mutable std::mutex queue_mutex_;
    std::size_t read_frame_{};
    std::size_t write_frame_{};
    std::size_t queued_frames_{};
    std::array<std::size_t, 16> recent_packet_frames_{};
    std::size_t packet_history_index_{};

    std::atomic<bool> playback_enabled_{true};
    std::atomic<std::uint32_t> volume_units_{10000};
    std::uint32_t current_gain_units_{};

    std::atomic<bool> active_{};
    std::atomic<std::size_t> quantum_frames_{};
    std::atomic<std::uint64_t> rendered_frames_{};
    std::atomic<std::uint64_t> dropped_frames_{};
    std::atomic<std::uint64_t> underruns_{};

    pw_thread_loop* loop_{};
    pw_stream* stream_{};
};

const pw_stream_events PipeWireAudioRenderer::stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = &PipeWireAudioRenderer::on_state_changed,
    .process = &PipeWireAudioRenderer::on_process,
};

} // namespace

std::unique_ptr<IAudioRenderer> make_pipewire_audio_renderer(
    const coremedia::AudioStreamBasicDescription& format,
    bool playback_enabled, float volume) {
    return std::make_unique<PipeWireAudioRenderer>(format, playback_enabled,
        volume);
}

} // namespace iPhoneMirror::audio
