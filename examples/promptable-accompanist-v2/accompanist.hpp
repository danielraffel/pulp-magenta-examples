// Promptable Accompanist V2 — a MIDI-conditioned generative instrument built on
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

#include <pulp/runtime/model_store.hpp>

#include <magentart/mlx_engine.h>
#include <magentart/ring_buffer.h>
#include <magentart/detail/autorelease_pool.h>

#include <mlx/mlx.h>

#include "magenta_models.hpp"     // kMagentaSubsystem — shared model store lookup
#include "magenta_resources.hpp"  // shared_resources_dir/_complete — resources resolution

#include "accompanist_params.hpp"
#include "freeze_loop_sampler.hpp"

#include "accompanist_root.hpp"  // the instrument editor (faders + prompt) + "need a model" gate
#include "model_section.hpp"     // the Models tab V2 contributes to the host Settings

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pulp::examples::accompanist_v2 {

inline std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}
inline std::string default_resources() {
    if (const char* e = std::getenv("MRT2_RESOURCES"); e && *e) return e;
    // Prefer the shared store (~/.pulp/magenta/resources) when the in-plugin overlay
    // has downloaded the full set — that's what a plugin-only install populates. Fall
    // back to the legacy ~/Documents/Magenta layout otherwise.
    if (magenta_demo::shared_resources_complete())
        return magenta_demo::shared_resources_dir().string();
    return (std::filesystem::path(env_or("HOME", "")) /
            "Documents/Magenta/magenta-rt-v2/resources").string();
}
// Resolve the model file. Explicit MRT2_MODEL wins; otherwise prefer the active
// model selected in Pulp's shared model store. For the legacy ~/Documents/Magenta
// path, prefer mrt2_small for broad Apple Silicon compatibility; users can still
// opt into mrt2_base explicitly from the Models tab.
inline std::string default_model() {
    if (const char* e = std::getenv("MRT2_MODEL"); e && *e) return e;

    // Prefer a model installed through the in-plugin / standalone Models overlay — i.e. the
    // shared Pulp model store at ~/.pulp/magenta. That is what a plugin-only install
    // downloads (the user may never touch the legacy ~/Documents/Magenta layout), so the
    // engine must look there first. Require the `_state.safetensors` sibling too: a model
    // is only loadable as a complete bundle, and gating on it keeps a pre-existing legacy
    // install selected if the shared copy is a partial (e.g. weights-only) download.
    namespace rt = pulp::runtime;
    if (const std::string active = rt::read_active_model_id(magenta_demo::kMagentaSubsystem);
        !active.empty()) {
        const auto rec = rt::read_installed_model(magenta_demo::kMagentaSubsystem, active);
        if (rec.checkpoint_exists && !rec.resolved_checkpoint_path.empty()) {
            const auto& ckpt = rec.resolved_checkpoint_path;
            const auto state = ckpt.parent_path() / (ckpt.stem().string() + "_state.safetensors");
            std::error_code ec;
            if (std::filesystem::exists(state, ec)) return ckpt.string();
        }
    }

    // Legacy install path (scripts/install-weights.sh). Prefer the small model so a copied
    // app defaults to the most compatible complete checkpoint on M1-class machines.
    const auto root = std::filesystem::path(env_or("HOME", "")) /
                      "Documents/Magenta/magenta-rt-v2/models";
    const auto small = root / "mrt2_small" / "mrt2_small.mlxfn";
    const auto base  = root / "mrt2_base"  / "mrt2_base.mlxfn";
    std::error_code ec;
    if (std::filesystem::exists(small, ec)) return small.string();
    return base.string();
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
    std::atomic<bool> running{true}, loaded{false}, loading{false};
    std::atomic<bool> load_failed{false}, generation_failed{false};
    std::atomic<std::uint64_t> generated_frames{0}, underrun_blocks{0};
    std::atomic<float> last_frame_ms{0.0f};

    // Live model hot-reload. MLX makes streams/encoders thread_local, so the
    // model swap MUST run on the worker thread, not in the UI callback that asks
    // for it. The UI publishes the new checkpoint path + raises a flag; the worker
    // services it at a frame boundary (see worker_run). The prompt is mirrored
    // here so it survives a reload (a fresh load_model resets the engine prompt).
    std::atomic<bool> reload_requested{false};
    std::mutex mutex_;                 // guards pending_model_path + current_prompt
    std::string pending_model_path;
    std::string current_prompt;

    void request_reload(std::string checkpoint_path) {
        { std::lock_guard<std::mutex> lk(mutex_); pending_model_path = std::move(checkpoint_path); }
        reload_requested.store(true, std::memory_order_release);
    }
    // Set the text prompt on the engine (safe from any thread — the engine defers
    // the MLX text-encode to its worker) and remember it for the next reload.
    void set_prompt(const std::string& p) {
        { std::lock_guard<std::mutex> lk(mutex_); current_prompt = p; }
        engine.set_text_prompt(p);
    }
};

class Processor : public format::Processor {
public:
    ~Processor() override {
        freeze_sampler_.shutdown();
        if (st_) st_->running.store(false);
        if (worker_.joinable()) worker_.detach();   // instant teardown; worker self-exits
    }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PromptableAccompanistV2",
            .manufacturer = "PulpMagenta",
            .bundle_id = "com.pulp.magenta.accompanist.v2",
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
        store.add_parameter({kVolumeDb,        "Volume",      "dB", {-60.0f, 6.0f, 0.0f, 0.1f}});
        store.add_parameter({kFreeze,          "Freeze",      "",   {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({kCaptureSeconds,  "Capture",     "s",  {0.25f, 8.0f, 2.0f, 0.01f}});
        store.add_parameter({kLoopCrossfadeMs, "Loop XFade",  "ms", {0.0f, 100.0f, 30.0f, 1.0f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        FreezeLoopSamplerConfig sampler_config;
        sampler_config.num_channels = 2;
        sampler_config.sample_rate = ctx.sample_rate;
        sampler_config.max_block_frames = static_cast<std::uint32_t>(std::max(1, ctx.max_buffer_size));
        sampler_config.max_capture_seconds = 8.0;
        sampler_config.sample_slots = 2;
        freeze_sampler_.prepare(sampler_config);
        if (!worker_started_.exchange(true)) {
            st_ = std::make_shared<EngineState>();
            st_->current_prompt = env_or("MRT2_PROMPT", "warm analog pads");  // before worker loads
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
        const bool model_loaded = st_->loaded.load(std::memory_order_acquire);
        bool underrun = false;
        if (model_loaded) {
            const bool ok_l = st_->ring_l.read(L.data(), ns);
            const bool ok_r = st_->ring_r.read(R.data(), ns);
            underrun = !ok_l || !ok_r;
            if (underrun) st_->underrun_blocks.fetch_add(1, std::memory_order_relaxed);
        } else {
            for (std::size_t i = 0; i < ns; ++i) {
                L[i] = 0.0f;
                R[i] = 0.0f;
            }
        }

        FreezeLoopSamplerControls sampler_controls;
        sampler_controls.freeze = state().get_value(kFreeze) >= 0.5f;
        sampler_controls.capture_seconds = state().get_value(kCaptureSeconds);
        sampler_controls.loop_crossfade_ms = state().get_value(kLoopCrossfadeMs);
        freeze_sampler_.process(out, sampler_controls);

        const float gain = std::pow(10.0f, state().get_value(kVolumeDb) / 20.0f);
        for (std::size_t i = 0; i < ns; ++i) { L[i] *= gain; R[i] *= gain; }
        for (std::size_t ch = 2; ch < nch; ++ch) {
            auto s = out.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) s[i] = L[i];
        }
    }

    // GPU-native editor. Tall enough that the host Settings panel's Audio tab fits without
    // scrolling; the editor top-aligns in the extra space.
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
                if (id == kTopK)                  std::snprintf(buf, sizeof buf, "%d", (int)std::lround(v));
                else if (id == kTemperature)       std::snprintf(buf, sizeof buf, "%.2f", v);
                else if (id == kFreeze)            std::snprintf(buf, sizeof buf, "%s", v >= 0.5f ? "On" : "Off");
                else if (id == kCaptureSeconds)    std::snprintf(buf, sizeof buf, "%.2fs", v);
                else if (id == kLoopCrossfadeMs)   std::snprintf(buf, sizeof buf, "%.0fms", v);
                else                               std::snprintf(buf, sizeof buf, "%.1f", v);
                return buf;
            },
            [this](const std::string& p) { if (st_) st_->set_prompt(p); },
            [] { std::error_code ec; return std::filesystem::exists(default_model(), ec); },  // model_ready
            [this] { return runtime_status_text(); },
            // on_model_changed (in-editor Models overlay, DAW). Ask the worker to hot-reload
            // the newly-activated model from the shared store — it swaps live, no restart. We
            // must NOT rebuild the editor here (that would destroy the ModelSection mid-callback);
            // the overlay's Done button refreshes the editor safely.
            [this] { if (st_) st_->request_reload(default_model()); },
            env_or("MRT2_PROMPT", "warm analog pads"));
        editor_ = editor.get();
        return editor;
    }

    void on_view_closed(view::View&) override { editor_ = nullptr; }

    FreezeLoopSamplerStatus freeze_sampler_status() const noexcept {
        return freeze_sampler_.status();
    }

    std::string runtime_status_text() const {
        const auto st = st_;
        if (!st) return {};

        if (st->loading.load(std::memory_order_acquire))
            return "Loading Magenta model...";
        if (st->generation_failed.load(std::memory_order_acquire))
            return "Model generation stopped. Open Settings > Models and try redownloading Small.";
        if (st->load_failed.load(std::memory_order_acquire))
            return "Model failed to start. Open Settings > Models and try redownloading Small.";
        if (!st->loaded.load(std::memory_order_acquire))
            return {};

        const auto generated = st->generated_frames.load(std::memory_order_relaxed);
        if (generated < 3)
            return "Model loaded; warming up generated audio...";

        const auto underruns = st->underrun_blocks.load(std::memory_order_relaxed);
        if (underruns > 200 && st->ring_l.available() == 0)
            return "Generated audio is underrunning. Try Small, 48 kHz, and close other GPU-heavy apps.";

        return {};
    }

    // V2 contributes a "Models" tab to the host's unified Settings panel; the host composes
    // it alongside its own Audio/MIDI device tabs (Processor::settings_sections, MM-PR5).
    std::vector<format::Processor::SettingsSection> settings_sections() override {
        std::vector<format::Processor::SettingsSection> sections;
        sections.push_back({"Models", std::make_unique<magenta_demo::ModelSection>(
                                          [this] {
                                              if (st_) st_->request_reload(default_model());  // swap live
                                              if (editor_) editor_->refresh();
                                          })});
        return sections;
    }
private:
    // Load a checkpoint on THIS (worker) thread — required: MLX streams/encoders
    // are thread_local, so load_model must run where generate_frame runs. Returns
    // true once the model + its text/quantizer encoders are ready. Re-applies the
    // remembered prompt so a hot-reload doesn't drop it. assets_ok gates the very
    // first init_assets (resources are shared and loaded once).
    static bool worker_load(const std::shared_ptr<EngineState>& st, const std::string& path,
                            bool assets_ok) {
        using namespace std::chrono_literals;
        magentart::detail::AutoreleasePool pool;
        st->loading.store(true, std::memory_order_release);
        st->loaded.store(false, std::memory_order_release);
        st->load_failed.store(false, std::memory_order_release);
        st->generation_failed.store(false, std::memory_order_release);
        st->generated_frames.store(0, std::memory_order_relaxed);
        st->underrun_blocks.store(0, std::memory_order_relaxed);
        std::error_code ec;
        if (!assets_ok || path.empty() || !std::filesystem::exists(path, ec)) {
            st->loading.store(false, std::memory_order_release);
            st->load_failed.store(true, std::memory_order_release);
            return false;
        }
        if (st->engine.is_loaded()) st->engine.unload();   // swap: drop the old model first
        if (!st->engine.load_model(path.c_str())) {
            st->loading.store(false, std::memory_order_release);
            st->load_failed.store(true, std::memory_order_release);
            return false;
        }
        std::string prompt;
        { std::lock_guard<std::mutex> lk(st->mutex_); prompt = st->current_prompt; }
        st->engine.set_text_prompt(prompt);
        while (st->running.load() &&
               (st->engine.get_text_encoder_status() == 1 || st->engine.get_quantizer_status() == 1))
            std::this_thread::sleep_for(10ms);
        st->loading.store(false, std::memory_order_release);
        return true;
    }

    static bool generate_and_write_frame(const std::shared_ptr<EngineState>& st,
                                         float* L, float* R) {
        using namespace std::chrono_literals;
        namespace mc = magentart::core;
        {
            magentart::detail::AutoreleasePool pool;
            const auto start = std::chrono::steady_clock::now();
            if (!st->engine.generate_frame(L, R)) {
                st->generation_failed.store(true, std::memory_order_release);
                st->load_failed.store(true, std::memory_order_release);
                return false;
            }
            const auto end = std::chrono::steady_clock::now();
            st->last_frame_ms.store(
                std::chrono::duration<float, std::milli>(end - start).count(),
                std::memory_order_relaxed);
        }

        while (st->running.load(std::memory_order_relaxed) &&
               (st->ring_l.free_space() < mc::kFrameSamples ||
                st->ring_r.free_space() < mc::kFrameSamples)) {
            auto dummy = mlx::core::array({0.0f}) + mlx::core::array({0.0f});
            mlx::core::eval(dummy);
            std::this_thread::sleep_for(200us);
        }
        if (!st->running.load(std::memory_order_relaxed)) return false;
        if (!st->ring_l.write(L, mc::kFrameSamples) ||
            !st->ring_r.write(R, mc::kFrameSamples)) {
            st->generation_failed.store(true, std::memory_order_release);
            st->load_failed.store(true, std::memory_order_release);
            return false;
        }
        st->generated_frames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static bool prime_output_ring(const std::shared_ptr<EngineState>& st,
                                  float* L, float* R) {
        for (int i = 0; i < 3; ++i) {
            if (!generate_and_write_frame(st, L, R)) return false;
        }
        return true;
    }

    static void worker_run(std::shared_ptr<EngineState> st) {
        using namespace std::chrono_literals;
        namespace mc = magentart::core;

        bool assets_ok = false;
        {
            magentart::detail::AutoreleasePool pool;
            assets_ok = st->engine.init_assets(default_resources().c_str(), "musiccoca");
        }
        if (!assets_ok)
            std::fprintf(stderr, "[PromptableAccompanistV2] MRT2 shared resources not found at:\n"
                                 "    %s\n  The instrument stays silent until they are installed.\n",
                         default_resources().c_str());

        // Initial load. Unlike before, a missing model is NOT fatal — the worker idles
        // and keeps servicing reload requests, so a model downloaded later from the
        // in-plugin Models overlay starts playing live, with no restart.
        const std::string initial = default_model();
        std::error_code ec;
        if (assets_ok && !std::filesystem::exists(initial, ec))
            std::fprintf(stderr,
                "[PromptableAccompanistV2] No MRT2 model installed yet. Open Settings -> Models to\n"
                "  download one (mrt2_small or mrt2_base) — it will start playing without a restart.\n");
        float L[mc::kFrameSamples], R[mc::kFrameSamples];
        bool ok = worker_load(st, initial, assets_ok);
        if (ok) ok = prime_output_ring(st, L, R);
        st->loaded.store(ok, std::memory_order_release);
        st->load_failed.store(!ok, std::memory_order_release);

        while (st->running.load()) {
            // Service a hot-reload at a frame boundary (on this MLX-owning thread).
            if (st->reload_requested.exchange(false, std::memory_order_acquire)) {
                std::string path;
                { std::lock_guard<std::mutex> lk(st->mutex_); path = st->pending_model_path; }
                const bool rok_load = worker_load(st, path, assets_ok);
                bool rok = rok_load;
                if (rok) rok = prime_output_ring(st, L, R);
                st->loaded.store(rok, std::memory_order_release);
                st->load_failed.store(!rok, std::memory_order_release);
            }
            if (!st->loaded.load()) {               // no model yet → idle, stay responsive
                std::this_thread::sleep_for(20ms);
                continue;
            }
            if (!generate_and_write_frame(st, L, R)) {
                st->loaded.store(false, std::memory_order_release);
                std::fprintf(stderr,
                             "[PromptableAccompanistV2] MRT2 generation stopped; "
                             "the instrument will stay silent until the model is reloaded.\n");
            }
        }
    }

    std::shared_ptr<EngineState> st_;
    std::thread worker_;
    std::atomic<bool> worker_started_{false};
    double sample_rate_ = 48000.0;
    FreezeLoopSampler freeze_sampler_;
    magenta_demo::AccompanistRoot* editor_ = nullptr;  // refreshed when the active model changes
};

inline std::unique_ptr<format::Processor> create_promptable_accompanist_v2() {
    return std::make_unique<Processor>();
}

} // namespace pulp::examples::accompanist_v2
