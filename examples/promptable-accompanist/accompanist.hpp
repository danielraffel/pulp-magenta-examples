// Promptable Accompanist (E1) — a MIDI-conditioned generative instrument built on
// Magenta RealTime 2, exposed cross-format via Pulp (VST3 / AU / CLAP / Standalone).
//
// HONEST UX: MRT2 is generative with ~200 ms latency. Incoming MIDI *steers* the
// generation (density, pitch region, harmony, onsets) — it is NOT a deterministic synth
// voice that plays the exact notes you press. Text prompts + numeric controls shape the
// timbre/genre. See ../../docs/SETUP.md.
//
// THREADING — important. MLX (e9e20fa, required for Xcode-26 Metal compat) makes streams
// and Metal command encoders thread_local: arrays created on one thread can't be eval'd on
// another ("There is no Stream(gpu, N) in current thread"). So ALL MLX work — model load
// AND generation — runs on ONE dedicated worker thread here, feeding a lock-free ring
// buffer. process() only drains the ring (RT-safe) and pushes atomic params/MIDI to the
// engine. (This is why we drive MLXEngine directly rather than RealtimeRunner, whose
// load-on-controller / infer-on-worker split trips the thread_local-stream check.)
//
// Model assets resolve from env (defaults = the standard `mrt models` install path):
//   MRT2_MODEL  MRT2_RESOURCES  MRT2_PROMPT
#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <magentart/mlx_engine.h>
#include <magentart/ring_buffer.h>
#include <magentart/detail/autorelease_pool.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace pulp::examples::accompanist {

enum ParamId : state::ParamID {
    kTemperature = 0, kTopK, kCfgMusicCoCa, kCfgNotes, kCfgDrums, kVolumeDb,
};

inline std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}
inline std::string default_resources() {
    return env_or("MRT2_RESOURCES", (std::filesystem::path(env_or("HOME", "")) /
                  "Documents/Magenta/magenta-rt-v2/resources").string());
}
inline std::string default_model() {
    return env_or("MRT2_MODEL", (std::filesystem::path(env_or("HOME", "")) /
                  "Documents/Magenta/magenta-rt-v2/models/mrt2_small/mrt2_small.mlxfn").string());
}

class Processor : public format::Processor {
public:
    ~Processor() override { stop_worker(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PromptableAccompanist",
            .manufacturer = "PulpMagenta",
            .bundle_id = "com.pulp.magenta.accompanist",
            .version = "0.1.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = -1,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({kTemperature,  "Temperature", "",   {0.1f, 2.0f, 1.1f, 0.01f}});
        store.add_parameter({kTopK,         "Top K",       "",   {1.0f, 64.0f, 40.0f, 1.0f}});
        store.add_parameter({kCfgMusicCoCa, "Prompt CFG",  "",   {0.0f, 8.0f, 1.0f, 0.1f}});
        store.add_parameter({kCfgNotes,     "Notes CFG",   "",   {0.0f, 8.0f, 1.0f, 0.1f}});
        store.add_parameter({kCfgDrums,     "Drums CFG",   "",   {0.0f, 8.0f, 1.0f, 0.1f}});
        store.add_parameter({kVolumeDb,     "Volume",      "dB", {-60.0f, 6.0f, 0.0f, 0.1f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        ring_l_.set_virtual_capacity(magentart::core::RingBuffer::kCapacity);
        ring_r_.set_virtual_capacity(magentart::core::RingBuffer::kCapacity);
        if (!worker_started_.exchange(true)) {
            running_.store(true);
            worker_ = std::thread([this] { worker_run(); });
        }
    }

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        // Push atomic controls + MIDI to the engine (thread-safe from any thread).
        engine_.set_temperature(state().get_value(kTemperature));
        engine_.set_top_k(static_cast<int>(state().get_value(kTopK)));
        engine_.set_cfg_musiccoca(state().get_value(kCfgMusicCoCa));
        engine_.set_cfg_notes(state().get_value(kCfgNotes));
        engine_.set_cfg_drums(state().get_value(kCfgDrums));
        for (const auto& ev : midi_in) {
            if (ev.is_note_on() && ev.velocity() > 0) engine_.set_note_on(ev.note());
            else if (ev.is_note_off() || ev.is_note_on()) engine_.set_note_off(ev.note());
        }

        const std::size_t nch = out.num_channels();
        const std::size_t ns  = out.num_samples();
        if (nch == 0 || ns == 0) return;

        // Drain generated 48 kHz stereo from the ring (RT-safe; zero-padded on underrun).
        auto L = out.channel(0);
        auto R = out.channel(nch > 1 ? 1 : 0);
        if (!ring_l_.read(L.data(), ns)) { for (std::size_t i = 0; i < ns; ++i) L[i] = 0.0f; }
        if (!ring_r_.read(R.data(), ns)) { for (std::size_t i = 0; i < ns; ++i) R[i] = 0.0f; }

        const float gain = std::pow(10.0f, state().get_value(kVolumeDb) / 20.0f);
        for (std::size_t i = 0; i < ns; ++i) { L[i] *= gain; R[i] *= gain; }
        for (std::size_t ch = 2; ch < nch; ++ch) {
            auto s = out.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) s[i] = L[i];
        }
    }

private:
    void worker_run() {
        using namespace std::chrono_literals;
        namespace mc = magentart::core;
        {
            magentart::detail::AutoreleasePool pool;
            if (!engine_.init_assets(default_resources().c_str(), "musiccoca") ||
                !engine_.load_model(default_model().c_str())) {
                load_failed_.store(true);
                return;
            }
            engine_.set_text_prompt(env_or("MRT2_PROMPT", "warm analog pads"));
        }
        while (running_.load() &&
               (engine_.get_text_encoder_status() == 1 || engine_.get_quantizer_status() == 1))
            std::this_thread::sleep_for(10ms);
        loaded_.store(true);

        float L[mc::kFrameSamples], R[mc::kFrameSamples];
        const std::size_t headroom = mc::RingBuffer::kCapacity - 2 * mc::kFrameSamples;
        while (running_.load()) {
            if (ring_l_.available() > headroom) {            // ring nearly full → wait
                std::this_thread::sleep_for(2ms);
                continue;
            }
            magentart::detail::AutoreleasePool pool;
            if (!engine_.generate_frame(L, R)) break;
            ring_l_.write(L, mc::kFrameSamples);
            ring_r_.write(R, mc::kFrameSamples);
        }
    }
    void stop_worker() {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
    }

    magentart::core::MLXEngine engine_;
    magentart::core::RingBuffer ring_l_, ring_r_;
    std::thread worker_;
    std::atomic<bool> worker_started_{false}, running_{false}, loaded_{false}, load_failed_{false};
    double sample_rate_ = 48000.0;
};

inline std::unique_ptr<format::Processor> create_promptable_accompanist() {
    return std::make_unique<Processor>();
}

} // namespace pulp::examples::accompanist
