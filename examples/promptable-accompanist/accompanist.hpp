// Promptable Accompanist (E1) — a MIDI-conditioned generative instrument built on
// Magenta RealTime 2 (`magentart::core::RealtimeRunner`), exposed cross-format via Pulp.
//
// HONEST UX: MRT2 is generative with ~200 ms latency. Incoming MIDI *steers* the
// generation (density, pitch region, harmony, onsets) — it is NOT a deterministic synth
// voice that plays the exact notes you press. Text prompts + numeric controls shape the
// timbre/genre. See ../../docs/SETUP.md.
//
// Model assets are resolved from env vars (with the standard `mrt models` install path as
// the default), so the example runs against weights installed via scripts/install-weights.sh:
//   MRT2_MODEL      = path to <model>.mlxfn       (default mrt2_small)
//   MRT2_RESOURCES  = dir containing musiccoca/   (default ~/Documents/Magenta/...)
//   MRT2_PROMPT     = initial text prompt          (default "warm analog pads")
#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <magentart/realtime_runner.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace pulp::examples::accompanist {

enum ParamId : state::ParamID {
    kTemperature = 0,
    kTopK,
    kCfgMusicCoCa,
    kCfgNotes,
    kCfgDrums,
    kVolumeDb,
};

inline std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

inline std::string default_resources() {
    return env_or("MRT2_RESOURCES",
                  (std::filesystem::path(env_or("HOME", "")) /
                   "Documents/Magenta/magenta-rt-v2/resources").string());
}
inline std::string default_model() {
    return env_or("MRT2_MODEL",
                  (std::filesystem::path(env_or("HOME", "")) /
                   "Documents/Magenta/magenta-rt-v2/models/mrt2_small/mrt2_small.mlxfn").string());
}

class Processor : public format::Processor {
public:
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
        // All numeric controls are host-automatable (MRT2's atomic setters).
        store.add_parameter({kTemperature,  "Temperature", "",   {0.1f, 2.0f, 1.1f, 0.01f}});
        store.add_parameter({kTopK,         "Top K",       "",   {1.0f, 64.0f, 40.0f, 1.0f}});
        store.add_parameter({kCfgMusicCoCa, "Prompt CFG",  "",   {0.0f, 8.0f, 1.0f, 0.1f}});
        store.add_parameter({kCfgNotes,     "Notes CFG",   "",   {0.0f, 8.0f, 1.0f, 0.1f}});
        store.add_parameter({kCfgDrums,     "Drums CFG",   "",   {0.0f, 8.0f, 1.0f, 0.1f}});
        store.add_parameter({kVolumeDb,     "Volume",      "dB", {-60.0f, 6.0f, 0.0f, 0.1f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        // Lazy one-time model load on the controller thread (audio is stopped here).
        // Heavy (seconds); a production host would load asynchronously and show a
        // "loading…" state. For the example we block once.
        if (!loaded_.load()) {
            const std::string resources = default_resources();
            const std::string model = default_model();
            if (runner_.init_assets(resources.c_str()) &&
                runner_.load_model(model.c_str())) {
                runner_.set_text_prompt(env_or("MRT2_PROMPT", "warm analog pads"));
                runner_.start();
                loaded_.store(true);
            }
        }
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t nch = output.num_channels();
        const std::size_t ns = output.num_samples();

        // Push numeric params to the engine (atomic, RT-safe).
        runner_.set_temperature(state().get_value(kTemperature));
        runner_.set_top_k(static_cast<int>(state().get_value(kTopK)));
        runner_.set_cfg_musiccoca(state().get_value(kCfgMusicCoCa));
        runner_.set_cfg_notes(state().get_value(kCfgNotes));
        runner_.set_cfg_drums(state().get_value(kCfgDrums));
        runner_.set_volume_db(state().get_value(kVolumeDb));

        // MIDI steers the generation: note-on/off feed the per-frame conditioning.
        for (const auto& ev : midi_in) {
            if (ev.is_note_on() && ev.velocity() > 0) runner_.set_note_on(ev.note());
            else if (ev.is_note_off() || ev.is_note_on()) runner_.set_note_off(ev.note());
        }

        if (nch == 0 || ns == 0) return;

        if (!loaded_.load()) {
            for (std::size_t ch = 0; ch < nch; ++ch) {
                auto s = output.channel(ch);
                for (std::size_t i = 0; i < ns; ++i) s[i] = 0.0f;
            }
            return;
        }

        // Pull generated 48 kHz stereo. v0 is 48 kHz-only; a resampler is future work
        // (read ../../docs/SETUP.md "sample-rate policy"). Non-blocking → RT-safe.
        auto left = output.channel(0);
        auto right = output.channel(nch > 1 ? 1 : 0);
        runner_.read_audio_stereo(left.data(), right.data(), ns, /*blocking=*/false);
        for (std::size_t ch = 2; ch < nch; ++ch) {
            auto s = output.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) s[i] = left[i];
        }
    }

private:
    magentart::core::RealtimeRunner runner_;
    std::atomic<bool> loaded_{false};
    double sample_rate_ = 48000.0;
};

inline std::unique_ptr<format::Processor> create_promptable_accompanist() {
    return std::make_unique<Processor>();
}

} // namespace pulp::examples::accompanist
