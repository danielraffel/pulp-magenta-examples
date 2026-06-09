#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace pulp::examples::accompanist_v2 {

struct FreezeLoopSamplerConfig {
    std::uint32_t num_channels = 2;
    double sample_rate = 48000.0;
    std::uint32_t max_block_frames = 512;
    double max_capture_seconds = 16.0;
    std::uint32_t sample_slots = 2;
};

struct FreezeLoopSamplerControls {
    bool freeze = false;
    double capture_seconds = 2.0;
    double loop_crossfade_ms = 30.0;
};

struct FreezeLoopSamplerStatus {
    bool prepared = false;
    bool freeze_requested = false;
    bool materialize_pending = false;
    bool frozen = false;
    bool hold_active = false;
    std::uint64_t sample_frames = 0;
    std::uint64_t captures_completed = 0;
    std::uint64_t materialize_failures = 0;
    std::uint64_t frames_discarded_while_held = 0;
    std::uint64_t buffer_shape_mismatches = 0;
};

// Host lifecycle calls must be serialized against process(); process() owns the
// real-time state once prepare() succeeds.
class FreezeLoopSampler {
public:
    FreezeLoopSampler() = default;
    FreezeLoopSampler(const FreezeLoopSampler&) = delete;
    FreezeLoopSampler& operator=(const FreezeLoopSampler&) = delete;

    ~FreezeLoopSampler() { shutdown(); }

    bool prepare(const FreezeLoopSamplerConfig& config) {
        shutdown();
        if (config.num_channels == 0 || config.num_channels > kMaxChannels ||
            config.sample_rate <= 0.0 || !std::isfinite(config.sample_rate) ||
            config.max_block_frames == 0 || config.max_capture_seconds <= 0.0 ||
            !std::isfinite(config.max_capture_seconds)) {
            return false;
        }

        config_ = config;
        const auto max_frames = static_cast<std::uint64_t>(
            std::ceil(config.sample_rate * config.max_capture_seconds));
        if (max_frames == 0) return false;

        const auto capture_result = capture_.prepare_seconds(config.num_channels,
                                                             config.sample_rate,
                                                             config.max_capture_seconds);
        if (!capture_result.ok) return false;
        if (!store_.prepare({std::max<std::uint32_t>(config.sample_slots, 2u),
                             config.num_channels,
                             max_frames})) {
            capture_.reset();
            return false;
        }

        materialize_buffer_.resize(config.num_channels, static_cast<std::size_t>(max_frames));
        render_scratch_.resize(config.num_channels, config.max_block_frames);
        publish_ptrs_.assign(config.num_channels, nullptr);
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { worker_loop(); });
        prepared_.store(true, std::memory_order_release);
        return true;
    }

    void shutdown() noexcept {
        prepared_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        if (hold_active_.load(std::memory_order_acquire)) {
            capture_.end_hold();
            hold_active_.store(false, std::memory_order_release);
        }
        while (jobs_.try_pop()) {}
        while (events_.try_pop()) {}
        store_.release();
        capture_.reset();
        renderer_.reset();
        mode_ = Mode::Live;
        pending_sequence_ = 0;
        active_view_ = {};
        pending_cancelled_ = false;
        retry_freeze_after_pending_ = false;
        last_freeze_ = false;
        fade_position_ = 0;
        fade_frames_ = 0;
        sample_frames_.store(0, std::memory_order_relaxed);
        audio_safe_generation_.store(0, std::memory_order_release);
        freeze_requested_.store(false, std::memory_order_release);
        pending_.store(false, std::memory_order_release);
        frozen_.store(false, std::memory_order_release);
        captures_completed_.store(0, std::memory_order_relaxed);
        materialize_failures_.store(0, std::memory_order_relaxed);
        buffer_shape_mismatches_.store(0, std::memory_order_relaxed);
    }

    void process(audio::BufferView<float> live, const FreezeLoopSamplerControls& controls) noexcept {
        if (!prepared_.load(std::memory_order_acquire) || live.empty()) return;
        if (live.num_channels() != config_.num_channels ||
            live.num_samples() > config_.max_block_frames) {
            buffer_shape_mismatches_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const auto frames = static_cast<std::uint64_t>(live.num_samples());
        const bool want_freeze = controls.freeze;
        freeze_requested_.store(want_freeze, std::memory_order_release);

        consume_events();
        capture_.append(live, frames);

        if (want_freeze && (!last_freeze_ || retry_freeze_after_pending_)) {
            begin_freeze(controls);
        }
        if (!want_freeze && last_freeze_) begin_release();
        last_freeze_ = want_freeze;

        render_if_needed(live, frames);
    }

    FreezeLoopSamplerStatus status() const noexcept {
        return {
            prepared_.load(std::memory_order_acquire),
            freeze_requested_.load(std::memory_order_acquire),
            pending_.load(std::memory_order_acquire),
            frozen_.load(std::memory_order_acquire),
            hold_active_.load(std::memory_order_acquire),
            sample_frames_.load(std::memory_order_relaxed),
            captures_completed_.load(std::memory_order_relaxed),
            materialize_failures_.load(std::memory_order_relaxed),
            capture_.frames_discarded_while_held(),
            buffer_shape_mismatches_.load(std::memory_order_relaxed),
        };
    }

private:
    static constexpr std::size_t kMaxChannels = 16;

    enum class Mode : std::uint8_t {
        Live,
        FreezingPending,
        FadeToFrozen,
        Frozen,
        FadeToLive,
    };

    struct MaterializeJob {
        std::uint64_t sequence = 0;
        audio::RollingAudioCaptureSnapshot snapshot;
        double loop_crossfade_ms = 30.0;
    };

    struct MaterializeEvent {
        std::uint64_t sequence = 0;
        bool ok = false;
        std::uint64_t frames = 0;
        double loop_crossfade_ms = 30.0;
    };

    static double clamp_double(double value, double lo, double hi) noexcept {
        if (!std::isfinite(value)) return lo;
        return std::clamp(value, lo, hi);
    }

    void begin_freeze(const FreezeLoopSamplerControls& controls) noexcept {
        if (pending_.load(std::memory_order_acquire) ||
            hold_active_.load(std::memory_order_acquire)) {
            if (pending_cancelled_) retry_freeze_after_pending_ = true;
            return;
        }
        if (mode_ == Mode::FadeToFrozen || mode_ == Mode::Frozen) return;

        const auto requested_seconds = clamp_double(controls.capture_seconds,
                                                    0.05,
                                                    config_.max_capture_seconds);
        const auto requested_frames = static_cast<std::uint64_t>(
            std::max(1.0, std::ceil(requested_seconds * config_.sample_rate)));
        const auto snapshot = capture_.begin_hold_last(requested_frames);
        if (!snapshot.valid) {
            retry_freeze_after_pending_ = true;
            return;
        }

        const auto seq = ++pending_sequence_;
        const auto loop_crossfade_ms = clamp_double(controls.loop_crossfade_ms, 0.0, 250.0);
        MaterializeJob job{seq, snapshot, loop_crossfade_ms};
        if (!jobs_.try_push(job)) {
            capture_.end_hold();
            hold_active_.store(false, std::memory_order_release);
            retry_freeze_after_pending_ = true;
            return;
        }

        pending_cancelled_ = false;
        retry_freeze_after_pending_ = false;
        pending_.store(true, std::memory_order_release);
        hold_active_.store(true, std::memory_order_release);
        mode_ = Mode::FreezingPending;
    }

    void begin_release() noexcept {
        if (mode_ == Mode::Frozen || mode_ == Mode::FadeToFrozen) {
            mode_ = Mode::FadeToLive;
            fade_position_ = 0;
            fade_frames_ = default_transition_frames();
        } else if (mode_ == Mode::FreezingPending) {
            pending_cancelled_ = true;
            retry_freeze_after_pending_ = false;
            mode_ = Mode::Live;
            frozen_.store(false, std::memory_order_release);
        }
    }

    std::uint64_t default_transition_frames() const noexcept {
        return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(
            std::ceil(config_.sample_rate * 0.025)));
    }

    std::uint64_t loop_crossfade_frames(std::uint64_t sample_frames,
                                        double loop_crossfade_ms) const noexcept {
        if (sample_frames < 8 || loop_crossfade_ms <= 0.0) return 0;
        const auto max_xfade = std::max<std::uint64_t>(1, sample_frames / 4);
        const auto requested = static_cast<std::uint64_t>(
            std::ceil(config_.sample_rate * (loop_crossfade_ms / 1000.0)));
        return std::min<std::uint64_t>(max_xfade, requested);
    }

    void consume_events() noexcept {
        while (auto event = events_.try_pop()) {
            if (event->sequence != pending_sequence_) continue;

            pending_.store(false, std::memory_order_release);
            if (hold_active_.load(std::memory_order_acquire)) {
                capture_.end_hold();
                hold_active_.store(false, std::memory_order_release);
            }

            const bool want_freeze_now = freeze_requested_.load(std::memory_order_acquire);
            const bool cancelled = pending_cancelled_;
            pending_cancelled_ = false;
            if (cancelled) {
                if (!want_freeze_now) retry_freeze_after_pending_ = false;
                mode_ = Mode::Live;
                frozen_.store(false, std::memory_order_release);
                continue;
            }

            if (!event->ok) {
                materialize_failures_.fetch_add(1, std::memory_order_relaxed);
                retry_freeze_after_pending_ = false;
                mode_ = Mode::Live;
                frozen_.store(false, std::memory_order_release);
                continue;
            }
            if (!want_freeze_now) {
                retry_freeze_after_pending_ = false;
                mode_ = Mode::Live;
                frozen_.store(false, std::memory_order_release);
                continue;
            }

            const auto view = store_.read_published_view();
            if (!configure_renderer(view, event->loop_crossfade_ms)) {
                materialize_failures_.fetch_add(1, std::memory_order_relaxed);
                mode_ = Mode::Live;
                frozen_.store(false, std::memory_order_release);
                continue;
            }

            active_view_ = view;
            retry_freeze_after_pending_ = false;
            audio_safe_generation_.store(active_view_.generation, std::memory_order_release);
            sample_frames_.store(event->frames, std::memory_order_relaxed);
            captures_completed_.fetch_add(1, std::memory_order_relaxed);
            mode_ = Mode::FadeToFrozen;
            fade_position_ = 0;
            fade_frames_ = default_transition_frames();
            frozen_.store(true, std::memory_order_release);
        }
    }

    bool configure_renderer(const audio::PublishedSampleView& view,
                            double loop_crossfade_ms) noexcept {
        if (!view.valid || view.num_frames < 2 || view.num_channels == 0) return false;
        audio::LoopRegion region;
        region.start_frame = 0;
        region.end_frame = view.num_frames;
        region.crossfade_frames = loop_crossfade_frames(view.num_frames, loop_crossfade_ms);
        region.source_sample_rate = view.sample_rate;
        region.playback_mode = audio::LoopPlaybackMode::Forward;
        region.crossfade_curve = audio::LoopCrossfadeCurve::EqualPower;
        region.interpolation = audio::LoopInterpolationMode::Linear;
        region.snap_policy = audio::LoopSnapPolicy::ValueDirection;

        if (!renderer_.set_region(region, view.num_frames)) return false;
        renderer_.set_start_fade_frames(0);
        renderer_.set_stop_fade_frames(0);
        renderer_.set_playback_rate(view.sample_rate > 0.0 ? view.sample_rate / config_.sample_rate : 1.0);
        renderer_.start();
        return true;
    }

    void render_if_needed(audio::BufferView<float> live, std::uint64_t frames) noexcept {
        if (mode_ != Mode::FadeToFrozen && mode_ != Mode::Frozen && mode_ != Mode::FadeToLive) {
            return;
        }
        if (live.num_samples() > render_scratch_.num_samples() ||
            live.num_channels() > render_scratch_.num_channels()) {
            mode_ = Mode::Live;
            frozen_.store(false, std::memory_order_release);
            return;
        }

        std::array<const float*, kMaxChannels> source_ptrs{};
        if (active_view_.num_channels > source_ptrs.size() ||
            !store_.populate_channel_ptrs(active_view_, source_ptrs.data(), source_ptrs.size())) {
            mode_ = Mode::Live;
            frozen_.store(false, std::memory_order_release);
            return;
        }

        audio::BufferView<const float> source(source_ptrs.data(),
                                              active_view_.num_channels,
                                              static_cast<std::size_t>(active_view_.num_frames));
        auto scratch = render_scratch_.view().slice(0, live.num_samples());
        renderer_.render(source, scratch, frames);

        for (std::uint64_t i = 0; i < frames; ++i) {
            const auto gains = gains_for_next_frame();
            for (std::size_t ch = 0; ch < live.num_channels(); ++ch) {
                auto* out = live.channel_ptr(ch);
                const auto frozen_sample = ch < scratch.num_channels() ? scratch.channel_ptr(ch)[i] : 0.0f;
                out[i] = static_cast<float>(static_cast<double>(out[i]) * gains.live +
                                            static_cast<double>(frozen_sample) * gains.frozen);
            }
        }
    }

    struct MixGains {
        double live = 1.0;
        double frozen = 0.0;
    };

    MixGains gains_for_next_frame() noexcept {
        if (mode_ == Mode::Frozen) return {0.0, 1.0};
        if (fade_frames_ == 0) return mode_ == Mode::FadeToLive ? MixGains{1.0, 0.0}
                                                                 : MixGains{0.0, 1.0};
        const auto t = std::clamp(static_cast<double>(fade_position_) /
                                  static_cast<double>(fade_frames_),
                                  0.0,
                                  1.0);
        ++fade_position_;
        if (mode_ == Mode::FadeToFrozen) {
            if (fade_position_ >= fade_frames_) mode_ = Mode::Frozen;
            return {1.0 - t, t};
        }
        if (mode_ == Mode::FadeToLive) {
            if (fade_position_ >= fade_frames_) {
                mode_ = Mode::Live;
                frozen_.store(false, std::memory_order_release);
                renderer_.reset();
            }
            return {t, 1.0 - t};
        }
        return {0.0, 1.0};
    }

    void worker_loop() noexcept {
        using namespace std::chrono_literals;
        while (running_.load(std::memory_order_acquire)) {
            auto job = jobs_.try_pop();
            if (!job) {
                // Demo policy: light polling keeps this helper independent of a
                // reusable background-work primitive.
                std::this_thread::sleep_for(1ms);
                continue;
            }

            auto destination = materialize_buffer_.view().slice(
                0, static_cast<std::size_t>(job->snapshot.frame_count));
            const auto result = capture_.materialize_held(job->snapshot, destination);
            bool ok = result.status == audio::RollingAudioCaptureMaterializeStatus::Ok &&
                      result.frames_copied > 1;
            if (ok) {
                for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
                    publish_ptrs_[ch] = destination.channel_ptr(ch);
                }
                const audio::BufferView<const float> publish_view(publish_ptrs_.data(),
                                                                  destination.num_channels(),
                                                                  static_cast<std::size_t>(result.frames_copied));
                ok = store_.publish(publish_view,
                                    result.frames_copied,
                                    config_.sample_rate,
                                    audio_safe_generation_.load(std::memory_order_acquire));
            }
            MaterializeEvent event{job->sequence,
                                   ok,
                                   ok ? result.frames_copied : 0,
                                   job->loop_crossfade_ms};
            while (running_.load(std::memory_order_acquire) && !events_.try_push(event)) {
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    FreezeLoopSamplerConfig config_;
    audio::RollingAudioCaptureBuffer capture_;
    audio::PublishedSampleStore store_;
    audio::LoopRenderer renderer_;
    audio::Buffer<float> materialize_buffer_;
    audio::Buffer<float> render_scratch_;
    std::vector<const float*> publish_ptrs_;
    runtime::SpscQueue<MaterializeJob, 4> jobs_;
    runtime::SpscQueue<MaterializeEvent, 4> events_;
    std::thread worker_;

    audio::PublishedSampleView active_view_;
    std::uint64_t pending_sequence_ = 0;
    std::uint64_t fade_position_ = 0;
    std::uint64_t fade_frames_ = 0;
    Mode mode_ = Mode::Live;
    bool pending_cancelled_ = false;
    bool retry_freeze_after_pending_ = false;
    bool last_freeze_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> prepared_{false};
    std::atomic<bool> freeze_requested_{false};
    std::atomic<bool> pending_{false};
    std::atomic<bool> frozen_{false};
    std::atomic<bool> hold_active_{false};
    std::atomic<std::uint64_t> audio_safe_generation_{0};
    std::atomic<std::uint64_t> sample_frames_{0};
    std::atomic<std::uint64_t> captures_completed_{0};
    std::atomic<std::uint64_t> materialize_failures_{0};
    std::atomic<std::uint64_t> buffer_shape_mismatches_{0};
};

}  // namespace pulp::examples::accompanist_v2
