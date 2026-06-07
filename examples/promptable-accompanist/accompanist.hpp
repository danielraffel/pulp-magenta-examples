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

// The custom WebView UI needs an SDK built with -DPULP_BUILD_WEBVIEW=ON. Without it the
// plugin still works fully (audio + host/auto param UI); only the bespoke editor is
// omitted. The CMake option PROMPTABLE_WEBVIEW_UI defines ACCOMPANIST_WEBVIEW_UI.
#ifdef ACCOMPANIST_WEBVIEW_UI
#include "accompanist_ui.hpp"
#else
#include "accompanist_root.hpp"  // the instrument editor (faders + prompt) + "need a model" gate
#include "model_section.hpp"     // the Models tab E1 contributes to the host Settings
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
// Resolve the model file. Explicit MRT2_MODEL wins; otherwise PREFER the large
// model (mrt2_base, 2.4B — better quality, needs a Pro/Max Mac) when it's been
// downloaded, and fall back to mrt2_small (230M, any M-series). Weights are NOT
// bundled in the plugin (GB-scale, CC-BY-4.0 Google DeepMind) — the user installs
// them once to this shared path via scripts/install-weights.sh, and every Pulp
// Magenta plugin/app finds them here.
inline std::string default_model() {
    if (const char* e = std::getenv("MRT2_MODEL"); e && *e) return e;
    const auto root = std::filesystem::path(env_or("HOME", "")) /
                      "Documents/Magenta/magenta-rt-v2/models";
    const auto base  = root / "mrt2_base"  / "mrt2_base.mlxfn";   // large, preferred
    const auto small = root / "mrt2_small" / "mrt2_small.mlxfn";  // fallback
    std::error_code ec;
    if (std::filesystem::exists(base, ec)) return base.string();
    return small.string();
}

// Heap engine state shared between the Processor and its worker thread, so the
// Processor can be destroyed INSTANTLY (detach, no join) even while the worker is
// mid-model-load. The worker holds a shared_ptr ref, finishes its current op,
// sees `running=false`, and exits — releasing the state. This keeps host
// instantiation/teardown fast (auval re-instantiates many times; a join-on-load
// would make validation crawl / time out).
struct EngineState {
    magentart::core::MLXEngine engine;
    magentart::core::RingBuffer ring_l, ring_r;
    std::atomic<bool> running{true}, loaded{false}, load_failed{false};
};

class Processor : public format::Processor {
public:
    ~Processor() override {
        if (st_) st_->running.store(false);
        if (worker_.joinable()) worker_.detach();   // instant teardown; worker self-exits
    }

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
        if (!worker_started_.exchange(true)) {
            st_ = std::make_shared<EngineState>();
            st_->ring_l.set_virtual_capacity(magentart::core::RingBuffer::kCapacity);
            st_->ring_r.set_virtual_capacity(magentart::core::RingBuffer::kCapacity);
            auto st = st_;                                 // worker holds a ref
            worker_ = std::thread([st] { worker_run(st); });
        }
    }

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t nch = out.num_channels();
        const std::size_t ns  = out.num_samples();
        if (nch == 0 || ns == 0) return;
        if (!st_) { for (std::size_t ch = 0; ch < nch; ++ch) { auto s = out.channel(ch); for (std::size_t i = 0; i < ns; ++i) s[i] = 0.0f; } return; }

        // Push atomic controls + MIDI to the engine (thread-safe from any thread).
        st_->engine.set_temperature(state().get_value(kTemperature));
        st_->engine.set_top_k(static_cast<int>(state().get_value(kTopK)));
        st_->engine.set_cfg_musiccoca(state().get_value(kCfgMusicCoCa));
        st_->engine.set_cfg_notes(state().get_value(kCfgNotes));
        st_->engine.set_cfg_drums(state().get_value(kCfgDrums));
        for (const auto& ev : midi_in) {
            if (ev.is_note_on() && ev.velocity() > 0) st_->engine.set_note_on(ev.note());
            else if (ev.is_note_off() || ev.is_note_on()) st_->engine.set_note_off(ev.note());
        }

        // Drain generated 48 kHz stereo from the ring (RT-safe; zero-padded on underrun).
        auto L = out.channel(0);
        auto R = out.channel(nch > 1 ? 1 : 0);
        if (!st_->ring_l.read(L.data(), ns)) { for (std::size_t i = 0; i < ns; ++i) L[i] = 0.0f; }
        if (!st_->ring_r.read(R.data(), ns)) { for (std::size_t i = 0; i < ns; ++i) R[i] = 0.0f; }

        const float gain = std::pow(10.0f, state().get_value(kVolumeDb) / 20.0f);
        for (std::size_t i = 0; i < ns; ++i) { L[i] *= gain; R[i] *= gain; }
        for (std::size_t ch = 2; ch < nch; ++ch) {
            auto s = out.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) s[i] = L[i];
        }
    }

#ifdef ACCOMPANIST_WEBVIEW_UI
    // ── WebView UI (Phase 7) — prompt + numeric controls, bridged to the engine. ──
    format::ViewSize view_size() const override { return {520, 430, 420, 320, 1000, 760}; }

    std::unique_ptr<view::View> create_view() override {
        return std::make_unique<AccompanistRoot>(
            [this](std::uint32_t id, float v) { state().set_value(id, v); },
            [this](const std::string& p) { if (st_) st_->engine.set_text_prompt(p); });
    }
    void on_view_opened(view::View& root) override {
        static_cast<AccompanistRoot&>(root).pane().attach_if_needed();
    }
    void on_view_resized(view::View& root, std::uint32_t, std::uint32_t) override {
        static_cast<AccompanistRoot&>(root).pane().sync_to_host();
    }
    void on_view_closed(view::View& root) override {
        static_cast<AccompanistRoot&>(root).pane().detach_if_needed();
    }
#else
    // ── GPU-native editor (Track B) — the default. Skia-drawn widgets to Magenta's
    //    tokens; renders in every host AND is headlessly capturable. ──
    // Tall enough that the host Settings panel's Audio tab fits without scrolling (scrolling
    // detaches ComboBox dropdowns). The editor (faders) top-aligns in the extra space.
    format::ViewSize view_size() const override { return {560, 500, 460, 460, 1000, 820}; }

    std::unique_ptr<view::View> create_view() override {
        // Just the instrument editor (faders + prompt) or a "you need a model" gate.
        // Model management + audio/MIDI device settings live in the host's unified Settings
        // panel — Models is contributed via settings_sections() below; Audio/MIDI are the
        // host's own tabs (the host owns the audio device).
        auto editor = std::make_unique<magenta_demo::AccompanistRoot>(
            [this](std::uint32_t id, float v) { state().set_normalized(id, v); },
            [this](std::uint32_t id) { return state().get_normalized(id); },
            [this](std::uint32_t id) -> std::string {
                const float v = state().get_value(id);
                char buf[32];
                if (id == kTopK)            std::snprintf(buf, sizeof buf, "%d", (int)std::lround(v));
                else if (id == kTemperature) std::snprintf(buf, sizeof buf, "%.2f", v);
                else                         std::snprintf(buf, sizeof buf, "%.1f", v);
                return buf;
            },
            [this](const std::string& p) { if (st_) st_->engine.set_text_prompt(p); },
            [] { std::error_code ec; return std::filesystem::exists(default_model(), ec); },  // model_ready
            // on_model_changed (in-editor Models overlay, DAW). Must NOT rebuild the editor
            // here — that would destroy the ModelSection mid-callback; the overlay's Done
            // button refreshes the editor safely. Reloading the running engine from the shared
            // store is the next slice (engine-store integration); the download itself completes
            // here and is picked up on the next engine start.
            [] {},
            env_or("MRT2_PROMPT", "warm analog pads"));
        editor_ = editor.get();
        return editor;
    }

    void on_view_closed(view::View&) override { editor_ = nullptr; }

    // E1 contributes a "Models" tab to the host's unified Settings panel; the host composes
    // it alongside its own Audio/MIDI device tabs (Processor::settings_sections, MM-PR5).
    std::vector<format::Processor::SettingsSection> settings_sections() override {
        std::vector<format::Processor::SettingsSection> sections;
        sections.push_back({"Models", std::make_unique<magenta_demo::ModelSection>(
                                          [this] { if (editor_) editor_->refresh(); })});
        return sections;
    }
#endif

private:
    static void worker_run(std::shared_ptr<EngineState> st) {
        using namespace std::chrono_literals;
        namespace mc = magentart::core;
        {
            magentart::detail::AutoreleasePool pool;
            std::error_code ec;
            if (!std::filesystem::exists(default_model(), ec)) {
                std::fprintf(stderr,
                    "[PromptableAccompanist] MRT2 model not found at:\n    %s\n"
                    "  Install the weights once (downloaded to ~/Documents/Magenta/magenta-rt-v2):\n"
                    "    scripts/install-weights.sh mrt2_base     # large, 2.4B (Pro/Max)\n"
                    "    scripts/install-weights.sh mrt2_small    # 230M (any M-series)\n"
                    "  Or set MRT2_MODEL to a .mlxfn path. Weights are CC-BY-4.0 (Google DeepMind),\n"
                    "  not bundled in the plugin. The instrument stays silent until a model loads.\n",
                    default_model().c_str());
            }
            if (!st->engine.init_assets(default_resources().c_str(), "musiccoca") ||
                !st->engine.load_model(default_model().c_str())) {
                st->load_failed.store(true);
                return;
            }
            st->engine.set_text_prompt(env_or("MRT2_PROMPT", "warm analog pads"));
        }
        while (st->running.load() &&
               (st->engine.get_text_encoder_status() == 1 || st->engine.get_quantizer_status() == 1))
            std::this_thread::sleep_for(10ms);
        st->loaded.store(true);

        float L[mc::kFrameSamples], R[mc::kFrameSamples];
        const std::size_t headroom = mc::RingBuffer::kCapacity - 2 * mc::kFrameSamples;
        while (st->running.load()) {
            if (st->ring_l.available() > headroom) {        // ring nearly full → wait
                std::this_thread::sleep_for(2ms);
                continue;
            }
            magentart::detail::AutoreleasePool pool;
            if (!st->engine.generate_frame(L, R)) break;
            st->ring_l.write(L, mc::kFrameSamples);
            st->ring_r.write(R, mc::kFrameSamples);
        }
    }

    std::shared_ptr<EngineState> st_;
    std::thread worker_;
    std::atomic<bool> worker_started_{false};
    double sample_rate_ = 48000.0;
    magenta_demo::AccompanistRoot* editor_ = nullptr;  // refreshed when the active model changes
};

inline std::unique_ptr<format::Processor> create_promptable_accompanist() {
    return std::make_unique<Processor>();
}

} // namespace pulp::examples::accompanist
