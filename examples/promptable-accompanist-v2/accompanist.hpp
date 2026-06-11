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
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace pulp::examples::accompanist_v2 {

inline std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

inline std::string default_prompt() {
    return env_or("MRT2_PROMPT", "warm analog pads");
}

inline bool prompt_has_text_conditioning(std::string_view prompt) {
    return std::any_of(prompt.begin(), prompt.end(), [](unsigned char c) {
        return std::isspace(c) == 0;
    });
}

inline constexpr std::chrono::milliseconds kPromptApplyDebounce{500};

inline bool prompt_change_is_settled(std::chrono::steady_clock::time_point changed_at,
                                     std::chrono::steady_clock::time_point now) {
    return now >= changed_at + kPromptApplyDebounce;
}

inline void apply_prompt_to_engine(magentart::core::MLXEngine& engine,
                                   const std::string& prompt) {
    if (prompt_has_text_conditioning(prompt))
        engine.set_text_prompt(prompt);
    else
        engine.set_musiccoca_tokens_masked();
}

inline bool magenta_v2_debug_enabled() {
    const char* v = std::getenv("PULP_MAGENTA_V2_DEBUG");
    return v && *v && std::string(v) != "0";
}

inline void magenta_v2_debug_log(const std::string& message) {
    if (magenta_v2_debug_enabled())
        std::fprintf(stderr, "[PromptableAccompanistV2] %s\n", message.c_str());
}

inline std::string default_resources() {
    if (const char* e = std::getenv("MRT2_RESOURCES"); e && *e) return e;
    // Prefer the shared store (~/.pulp/magenta/resources) when the in-plugin overlay
    // has downloaded the required files — that's what a plugin-only install populates.
    // Use exact-size completeness only for repairs/download progress; runtime should
    // try a present resource set and let init_assets() be the final authority.
    if (magenta_demo::shared_resources_available())
        return magenta_demo::shared_resources_dir().string();
    if (magenta_demo::legacy_resources_available())
        return magenta_demo::legacy_resources_dir().string();
    return magenta_demo::shared_resources_dir().string();
}

inline bool model_bundle_complete(const std::filesystem::path& checkpoint) {
    return magenta_demo::magenta_model_bundle_complete(checkpoint);
}

inline bool env_truthy(const char* key) {
    const char* v = std::getenv(key);
    return v && *v && std::string(v) != "0";
}

inline bool package_audit_audio_muted() {
    if (!env_truthy("PULP_STANDALONE_PACKAGE_AUDIT")) return false;
    return !env_truthy("PULP_STANDALONE_PACKAGE_AUDIT_AUDIO") &&
           !env_truthy("PULP_MAGENTA_V2_PACKAGE_AUDIT_AUDIO");
}

inline bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

inline std::string host_hardware_model() {
    if (const char* override = std::getenv("PULP_MAGENTA_V2_HW_MODEL"); override && *override)
        return override;
#if defined(__APPLE__)
    char model[256]{};
    std::size_t size = sizeof(model);
    if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 && size > 0) {
        if (model[size - 1] == '\0') --size;
        return std::string(model, size);
    }
#endif
    return {};
}

inline bool host_is_m1_family() {
    const std::string model = host_hardware_model();
    return starts_with(model, "MacBookAir10,") ||
           starts_with(model, "MacBookPro17,") ||
           starts_with(model, "MacBookPro18,") ||
           starts_with(model, "Macmini9,") ||
           starts_with(model, "iMac21,") ||
           starts_with(model, "Mac13,");
}

inline std::string magenta_model_id_from_path(const std::filesystem::path& checkpoint) {
    const std::string generic = checkpoint.generic_string();
    if (generic.find("mrt2_base") != std::string::npos) return "mrt2_base";
    if (generic.find("mrt2_small") != std::string::npos) return "mrt2_small";
    return checkpoint.stem().string();
}

inline bool model_supported_for_realtime_host(const std::filesystem::path& checkpoint) {
    if (env_truthy("PULP_MAGENTA_V2_ALLOW_UNSUPPORTED_MODEL")) return true;
    const std::string model_id = magenta_model_id_from_path(checkpoint);
    if (model_id != "mrt2_base") return true;
    // Magenta's real-time device table says mrt2_base is not supported on M1 Pro/Air.
    // The M1 crash reports show MLX can abort during load, so block it before MLX sees it.
    return !host_is_m1_family();
}

inline bool model_bundle_usable(const std::filesystem::path& checkpoint) {
    return model_bundle_complete(checkpoint) && model_supported_for_realtime_host(checkpoint);
}

inline std::string complete_shared_model_path(const std::string& model_id) {
    namespace rt = pulp::runtime;
    const auto rec = rt::read_installed_model(magenta_demo::kMagentaSubsystem, model_id);
    if (!rec.metadata_found) return {};
    if (!model_bundle_complete(rec.resolved_checkpoint_path)) {
        magenta_v2_debug_log("shared model '" + model_id + "' is incomplete");
        return {};
    }
    if (!model_supported_for_realtime_host(rec.resolved_checkpoint_path)) {
        magenta_v2_debug_log("shared model '" + model_id + "' is not supported on this Mac");
        return {};
    }
    return rec.resolved_checkpoint_path.string();
}

inline std::string complete_legacy_model_path(const std::filesystem::path& checkpoint) {
    if (!model_bundle_complete(checkpoint)) return {};
    if (!model_supported_for_realtime_host(checkpoint)) {
        magenta_v2_debug_log("legacy model '" + checkpoint.string() + "' is not supported on this Mac");
        return {};
    }
    return checkpoint.string();
}

// Resolve the model file. Explicit MRT2_MODEL wins; otherwise prefer the active
// model selected in Pulp's shared model store. For the legacy ~/Documents/Magenta
// path, prefer mrt2_small for broad Apple Silicon compatibility; users can still
// opt into mrt2_base explicitly from the Models tab.
inline std::string default_model() {
    if (const char* e = std::getenv("MRT2_MODEL"); e && *e) {
        const std::filesystem::path explicit_path(e);
        if (model_bundle_usable(explicit_path)) return explicit_path.string();
        magenta_v2_debug_log("explicit MRT2_MODEL is not complete or supported: '" +
                             explicit_path.string() + "'");
        return {};
    }

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
        magenta_v2_debug_log("active model '" + active + "' resolves to '" +
                             rec.resolved_checkpoint_path.string() + "'");
        if (rec.metadata_found && model_bundle_usable(rec.resolved_checkpoint_path))
            return rec.resolved_checkpoint_path.string();
        magenta_v2_debug_log("active model '" + active + "' is incomplete or unsupported");
    }

    if (auto shared_small = complete_shared_model_path("mrt2_small"); !shared_small.empty())
        return shared_small;
    if (auto shared_base = complete_shared_model_path("mrt2_base"); !shared_base.empty())
        return shared_base;

    // Legacy install path (scripts/install-weights.sh). Prefer the small model so a copied
    // app defaults to the most compatible complete checkpoint on M1-class machines.
    const auto root = magenta_demo::legacy_magenta_models_root();
    const auto small = root / "mrt2_small" / "mrt2_small.mlxfn";
    const auto base  = root / "mrt2_base"  / "mrt2_base.mlxfn";
    if (auto legacy_small = complete_legacy_model_path(small); !legacy_small.empty())
        return legacy_small;
    if (auto legacy_base = complete_legacy_model_path(base); !legacy_base.empty())
        return legacy_base;
    return {};
}

enum class RuntimeIssue : std::uint32_t {
    none = 0,
    missing_resources,
    missing_model_bundle,
    unsupported_model,
    model_load_failed,
    encoder_failed,
    generation_failed,
};

inline constexpr std::uint32_t issue_value(RuntimeIssue issue) {
    return static_cast<std::uint32_t>(issue);
}

enum class WorkerLoadOutcome {
    loaded,
    preserved_current_model,
    failed,
};

inline constexpr int kGenerationStagnationFrameLimit = 12;
inline constexpr float kGenerationWatchdogEnergyFloor = 1.0e-5f;

// Heap engine state shared between the Processor and its worker thread. The worker
// owns every MLXEngine call and is joined during Processor teardown so the engine
// cannot be destroyed while Magenta's asynchronous prompt encoder is still running.
struct EngineState {
    magentart::core::MLXEngine engine;
    magentart::core::RingBuffer ring_l, ring_r;
    std::atomic<bool> running{true}, loaded{false}, loading{false};
    std::atomic<bool> loading_model_candidate_valid{false};
    std::atomic<bool> load_failed{false}, generation_failed{false};
    std::atomic<std::uint32_t> runtime_issue{issue_value(RuntimeIssue::none)};
    std::atomic<std::uint64_t> generated_frames{0}, underrun_blocks{0};
    std::atomic<float> last_frame_ms{0.0f};
    std::atomic<float> temperature{1.1f};
    std::atomic<int> top_k{40};
    std::atomic<float> cfg_musiccoca{1.0f};
    std::atomic<float> cfg_notes{1.0f};
    std::atomic<float> cfg_drums{1.0f};
    std::array<std::atomic<bool>, 128> desired_notes{};
    std::array<bool, 128> applied_notes{};
    std::atomic<std::uint64_t> prompt_revision{1};
    std::atomic<std::uint64_t> applied_prompt_revision{0};
    std::uint64_t previous_generation_signature = 0;
    int repeated_generation_frames = 0;
    std::atomic<std::uint64_t> generation_watchdog_resets{0};

    // Live model hot-reload. MLX makes streams/encoders thread_local, so the
    // model swap MUST run on the worker thread, not in the UI callback that asks
    // for it. The UI publishes the new checkpoint path + raises a flag; the worker
    // services it at a frame boundary (see worker_run). The prompt is mirrored
    // here so it survives a reload (a fresh load_model resets the engine prompt).
    std::atomic<bool> reload_requested{false};
    std::mutex mutex_;                 // guards pending_model_path/current_prompt/loaded_model_path
    std::string pending_model_path;
    std::string current_prompt;
    std::chrono::steady_clock::time_point prompt_changed_at = std::chrono::steady_clock::now();
    std::string loaded_model_path;

    void request_reload(std::string checkpoint_path) {
        magenta_v2_debug_log("reload requested for '" + checkpoint_path + "'");
        { std::lock_guard<std::mutex> lk(mutex_); pending_model_path = std::move(checkpoint_path); }
        reload_requested.store(true, std::memory_order_release);
    }

    void clear_loaded_model(RuntimeIssue issue = RuntimeIssue::missing_model_bundle) {
        magenta_v2_debug_log("clearing loaded model state");
        {
            std::lock_guard<std::mutex> lk(mutex_);
            pending_model_path.clear();
            loaded_model_path.clear();
        }
        reload_requested.store(false, std::memory_order_release);
        loading.store(false, std::memory_order_release);
        loading_model_candidate_valid.store(false, std::memory_order_release);
        loaded.store(false, std::memory_order_release);
        generation_failed.store(false, std::memory_order_release);
        runtime_issue.store(issue_value(issue), std::memory_order_release);
        load_failed.store(true, std::memory_order_release);
        generated_frames.store(0, std::memory_order_relaxed);
        underrun_blocks.store(0, std::memory_order_relaxed);
    }
    // The worker is the only thread that calls MLXEngine. UI/audio threads publish
    // desired prompt/control state here; worker_run applies it before generation.
    void set_prompt(const std::string& p) {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            current_prompt = p;
            prompt_changed_at = now;
        }
        prompt_revision.fetch_add(1, std::memory_order_release);
    }
};

inline bool generated_frame_has_energy(const float* left,
                                       const float* right,
                                       std::size_t frames) {
    const std::size_t stride = std::max<std::size_t>(1, frames / 64);
    for (std::size_t i = 0; i < frames; i += stride) {
        if (std::fabs(left[i]) > kGenerationWatchdogEnergyFloor ||
            std::fabs(right[i]) > kGenerationWatchdogEnergyFloor)
            return true;
    }
    return false;
}

inline std::uint64_t generated_frame_signature(const float* left,
                                               const float* right,
                                               std::size_t frames) {
    constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    std::uint64_t hash = kFnvOffset;
    const std::size_t stride = std::max<std::size_t>(1, frames / 64);
    auto mix_sample = [&](float sample) {
        const float clamped = std::clamp(sample, -2.0f, 2.0f);
        const auto quantized = static_cast<std::int32_t>(std::lrint(clamped * 32767.0f));
        const auto bits = static_cast<std::uint32_t>(quantized);
        hash ^= bits & 0xffu;
        hash *= kFnvPrime;
        hash ^= (bits >> 8u) & 0xffu;
        hash *= kFnvPrime;
        hash ^= (bits >> 16u) & 0xffu;
        hash *= kFnvPrime;
        hash ^= (bits >> 24u) & 0xffu;
        hash *= kFnvPrime;
    };
    for (std::size_t i = 0; i < frames; i += stride) {
        mix_sample(left[i]);
        mix_sample(right[i]);
    }
    return hash;
}

inline void reset_generation_stagnation_watchdog(EngineState& st) {
    st.previous_generation_signature = 0;
    st.repeated_generation_frames = 0;
}

inline bool update_generation_stagnation_watchdog(EngineState& st,
                                                  const float* left,
                                                  const float* right,
                                                  std::size_t frames) {
    if (!generated_frame_has_energy(left, right, frames)) {
        reset_generation_stagnation_watchdog(st);
        return false;
    }

    const auto signature = generated_frame_signature(left, right, frames);
    if (signature == st.previous_generation_signature) {
        ++st.repeated_generation_frames;
    } else {
        st.previous_generation_signature = signature;
        st.repeated_generation_frames = 0;
    }

    if (st.repeated_generation_frames < kGenerationStagnationFrameLimit)
        return false;

    reset_generation_stagnation_watchdog(st);
    return true;
}

class Processor : public format::Processor {
public:
    ~Processor() override {
        shutdown_worker();
        freeze_sampler_.shutdown();
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
            st_->current_prompt = default_prompt();  // before worker loads
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

        // Publish desired controls + MIDI for the MLX worker thread. The audio thread
        // must not call MLXEngine directly while the worker can be loading/reloading.
        st_->temperature.store(state().get_value(kTemperature), std::memory_order_relaxed);
        st_->top_k.store(static_cast<int>(state().get_value(kTopK)), std::memory_order_relaxed);
        st_->cfg_musiccoca.store(state().get_value(kCfgMusicCoCa), std::memory_order_relaxed);
        st_->cfg_notes.store(state().get_value(kCfgNotes), std::memory_order_relaxed);
        st_->cfg_drums.store(state().get_value(kCfgDrums), std::memory_order_relaxed);
        for (const auto& ev : midi_in) {
            const int note = ev.note();
            if (note < 0 || note >= static_cast<int>(st_->desired_notes.size())) continue;
            if (ev.is_note_on() && ev.velocity() > 0)
                st_->desired_notes[static_cast<std::size_t>(note)].store(true, std::memory_order_relaxed);
            else if (ev.is_note_off() || ev.is_note_on())
                st_->desired_notes[static_cast<std::size_t>(note)].store(false, std::memory_order_relaxed);
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
        sampler_controls.midi = &midi_in;
        sampler_controls.root_note = 60;
        freeze_sampler_.process(out, sampler_controls);

        const float gain = std::pow(10.0f, state().get_value(kVolumeDb) / 20.0f);
        for (std::size_t i = 0; i < ns; ++i) { L[i] *= gain; R[i] *= gain; }
        for (std::size_t ch = 2; ch < nch; ++ch) {
            auto s = out.channel(ch);
            for (std::size_t i = 0; i < ns; ++i) s[i] = L[i];
        }

        if (package_audit_audio_muted()) {
            for (std::size_t ch = 0; ch < nch; ++ch) {
                auto s = out.channel(ch);
                for (std::size_t i = 0; i < ns; ++i) s[i] = 0.0f;
            }
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
            [this] { return model_ready_for_editor(); },  // model_ready
            [this] { return runtime_status_text(); },
            // on_model_changed (in-editor Models overlay, DAW). Ask the worker to hot-reload
            // the newly-activated model from the shared store — it swaps live, no restart. We
            // must NOT rebuild the editor here (that would destroy the ModelSection mid-callback);
            // the overlay's Done button refreshes the editor safely.
            [this] { request_reload_to_default_model(); },
            current_prompt_for_view());
        editor_ = editor.get();
        return editor;
    }

    void on_view_closed(view::View&) override { editor_ = nullptr; }

    FreezeLoopSamplerStatus freeze_sampler_status() const noexcept {
        return freeze_sampler_.status();
    }

#ifdef PROMPTABLE_ACCOMPANIST_V2_TESTING
    void request_model_reload_for_test(const std::string& checkpoint_path) {
        if (st_) st_->request_reload(checkpoint_path);
    }

    std::string loaded_model_path_for_test() {
        const auto st = st_;
        if (!st) return {};
        std::lock_guard<std::mutex> lk(st->mutex_);
        return st->loaded_model_path;
    }

    bool model_ready_for_editor_for_test() const {
        return model_ready_for_editor();
    }

    void set_prompt_for_test(const std::string& prompt) {
        if (st_) st_->set_prompt(prompt);
    }

    void set_engine_state_for_test(std::shared_ptr<EngineState> st) {
        st_ = std::move(st);
    }

    static WorkerLoadOutcome worker_load_for_test(const std::shared_ptr<EngineState>& st,
                                                  const std::string& checkpoint_path,
                                                  bool assets_ok) {
        return worker_load(st, checkpoint_path, assets_ok);
    }

    bool prompt_change_applied_for_test() const {
        const auto st = st_;
        if (!st) return false;
        return st->applied_prompt_revision.load(std::memory_order_acquire) ==
               st->prompt_revision.load(std::memory_order_acquire);
    }

    void force_runtime_flags_for_test(bool loaded,
                                      bool loading,
                                      bool load_failed,
                                      RuntimeIssue issue,
                                      std::uint64_t generated_frames,
                                      bool loading_model_candidate_valid = false) {
        if (!st_) st_ = std::make_shared<EngineState>();
        st_->loaded.store(loaded, std::memory_order_release);
        st_->loading.store(loading, std::memory_order_release);
        st_->loading_model_candidate_valid.store(loading_model_candidate_valid,
                                                 std::memory_order_release);
        st_->load_failed.store(load_failed, std::memory_order_release);
        st_->generation_failed.store(issue == RuntimeIssue::generation_failed,
                                     std::memory_order_release);
        st_->runtime_issue.store(issue_value(issue), std::memory_order_release);
        st_->generated_frames.store(generated_frames, std::memory_order_relaxed);
    }
#endif

    std::string runtime_status_text() const {
        const auto st = st_;
        if (!st) return {};

        const bool loaded = st->loaded.load(std::memory_order_acquire);
        if (st->generation_failed.load(std::memory_order_acquire))
            return "Model generation stopped. Open Settings > Models to reload or redownload the active model.";
        if (st->load_failed.load(std::memory_order_acquire)) {
            switch (static_cast<RuntimeIssue>(st->runtime_issue.load(std::memory_order_acquire))) {
                case RuntimeIssue::missing_resources:
                    if (loaded) return {};
                    return "Magenta resources are incomplete. Open Settings > Models to repair the install.";
                case RuntimeIssue::missing_model_bundle:
                    if (loaded)
                        return "You need to download or repair a model in Settings > Models to keep generating audio.";
                    return "You need to download a model in Settings > Models to start generating audio.";
                case RuntimeIssue::unsupported_model:
                    return "This Mac can run the Small model in real time. Open Settings > Models to download or select Small.";
                case RuntimeIssue::encoder_failed:
                    return "Model encoders failed to start. Open Settings > Models to reload or redownload the active model.";
                case RuntimeIssue::generation_failed:
                    return "Model generation stopped. Open Settings > Models to reload or redownload the active model.";
                case RuntimeIssue::model_load_failed:
                case RuntimeIssue::none:
                    return "Model failed to start. Open Settings > Models to reload or redownload the active model.";
            }
        }
        if (!loaded) {
            if (st->loading.load(std::memory_order_acquire)) {
                if (!st->loading_model_candidate_valid.load(std::memory_order_acquire))
                    return "You need to download a model in Settings > Models to start generating audio.";
                return "Loading Magenta model...";
            }
            return {};
        }

        const auto generated = st->generated_frames.load(std::memory_order_relaxed);
        if (generated < 3)
            return "Model loaded; warming up generated audio...";

        const auto underruns = st->underrun_blocks.load(std::memory_order_relaxed);
        if (underruns > 200 && st->ring_l.available() == 0)
            return "Generated audio is underrunning. Try the Small model, 48 kHz, and close other GPU-heavy apps.";

        return {};
    }

    // V2 contributes a "Models" tab to the host's unified Settings panel; the host composes
    // it alongside its own Audio/MIDI device tabs (Processor::settings_sections, MM-PR5).
    std::vector<format::Processor::SettingsSection> settings_sections() override {
        std::vector<format::Processor::SettingsSection> sections;
        sections.push_back({"Models", std::make_unique<magenta_demo::ModelSection>(
                                          [this] {
                                              request_reload_to_default_model();  // swap live
                                              if (editor_) editor_->refresh();
                                          })});
        return sections;
    }
private:
    void shutdown_worker() {
        if (st_) st_->running.store(false, std::memory_order_release);
        if (!worker_.joinable()) return;
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
            return;
        }
        worker_.join();
        worker_started_.store(false, std::memory_order_release);
    }

    bool model_ready_for_editor() const {
        const auto st = st_;
        if (st && st->loading.load(std::memory_order_acquire))
            return st->loading_model_candidate_valid.load(std::memory_order_acquire);
        if (st && st->loaded.load(std::memory_order_acquire)) {
            std::string loaded_path;
            { std::lock_guard<std::mutex> lk(st->mutex_); loaded_path = st->loaded_model_path; }
            if (!loaded_path.empty() && model_bundle_usable(loaded_path))
                return true;
        }
        return !default_model().empty();
    }

    void request_reload_to_default_model() {
        if (!st_) return;
        const auto path = default_model();
        if (path.empty()) {
            st_->clear_loaded_model(RuntimeIssue::missing_model_bundle);
            if (editor_) editor_->refresh();
            return;
        }
        st_->request_reload(path);
    }

    std::string current_prompt_for_view() const {
        const auto st = st_;
        if (!st) return default_prompt();
        std::lock_guard<std::mutex> lk(st->mutex_);
        return st->current_prompt;
    }

    static bool prompt_encoding_in_flight(const std::shared_ptr<EngineState>& st) {
        return st->engine.get_text_encoder_status() == 1 ||
               st->engine.get_quantizer_status() == 1;
    }

    static bool wait_for_prompt_encoding_idle(const std::shared_ptr<EngineState>& st,
                                              std::string_view context,
                                              std::chrono::milliseconds timeout,
                                              bool timeout_only_while_running) {
        using namespace std::chrono_literals;
        const auto start = std::chrono::steady_clock::now();
        bool logged_shutdown_wait = false;
        bool logged_slow_wait = false;
        while (prompt_encoding_in_flight(st)) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - start;
            const bool running = st->running.load(std::memory_order_acquire);
            if ((!timeout_only_while_running || running) && elapsed > timeout) {
                magenta_v2_debug_log(std::string(context) + " timed out");
                return false;
            }
            if (!running && !logged_shutdown_wait && elapsed > 250ms) {
                magenta_v2_debug_log(std::string(context) +
                                     " still running during shutdown; waiting before engine teardown");
                logged_shutdown_wait = true;
            } else if (running && !logged_slow_wait && elapsed > 5s) {
                magenta_v2_debug_log(std::string(context) + " still running");
                logged_slow_wait = true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }

    static void wait_for_prompt_encoding_idle_before_teardown(
        const std::shared_ptr<EngineState>& st) {
        // Magenta's MusicCoCa path owns a detached TFLite thread internally. If
        // MLXEngine is destroyed first, that thread can jump through a freed
        // interpreter/tokenizer pointer on app close.
        (void)wait_for_prompt_encoding_idle(
            st,
            "prompt encoder",
            std::chrono::hours(24),
            false);
    }

    static void apply_realtime_inputs(const std::shared_ptr<EngineState>& st) {
        st->engine.set_temperature(st->temperature.load(std::memory_order_relaxed));
        st->engine.set_top_k(st->top_k.load(std::memory_order_relaxed));
        st->engine.set_cfg_musiccoca(st->cfg_musiccoca.load(std::memory_order_relaxed));
        st->engine.set_cfg_notes(st->cfg_notes.load(std::memory_order_relaxed));
        st->engine.set_cfg_drums(st->cfg_drums.load(std::memory_order_relaxed));

        for (std::size_t i = 0; i < st->desired_notes.size(); ++i) {
            const bool desired = st->desired_notes[i].load(std::memory_order_relaxed);
            if (desired == st->applied_notes[i]) continue;
            st->applied_notes[i] = desired;
            if (desired)
                st->engine.set_note_on(static_cast<int>(i));
            else
                st->engine.set_note_off(static_cast<int>(i));
        }

        const auto revision = st->prompt_revision.load(std::memory_order_acquire);
        if (revision != st->applied_prompt_revision.load(std::memory_order_acquire)) {
            std::string prompt;
            std::chrono::steady_clock::time_point changed_at;
            {
                std::lock_guard<std::mutex> lk(st->mutex_);
                prompt = st->current_prompt;
                changed_at = st->prompt_changed_at;
            }
            if (!prompt_change_is_settled(changed_at, std::chrono::steady_clock::now()))
                return;
            apply_prompt_to_engine(st->engine, prompt);
            st->applied_prompt_revision.store(revision, std::memory_order_release);
        }
    }

    static void recover_stagnant_generation(const std::shared_ptr<EngineState>& st) {
        std::string prompt;
        {
            std::lock_guard<std::mutex> lk(st->mutex_);
            prompt = st->current_prompt;
        }
        apply_prompt_to_engine(st->engine, prompt);
        st->applied_prompt_revision.store(st->prompt_revision.load(std::memory_order_acquire),
                                          std::memory_order_release);

        const float base = std::clamp(st->temperature.load(std::memory_order_relaxed),
                                      0.1f,
                                      2.0f);
        const float nudged = base < 1.99f ? base + 0.01f : base - 0.01f;
        st->engine.set_temperature(nudged);
        st->engine.set_temperature(base);
        st->generation_watchdog_resets.fetch_add(1, std::memory_order_relaxed);
        magenta_v2_debug_log("generation watchdog nudged sampler after repeated output frames");
    }

    // Load a checkpoint on THIS (worker) thread — required: MLX streams/encoders
    // are thread_local, so load_model must run where generate_frame runs. The
    // outcome distinguishes destructive failures from rejected reload requests:
    // incomplete resources/bundles should preserve the currently loaded model.
    // Re-applies the remembered prompt so a hot-reload doesn't drop it. assets_ok
    // gates the very first init_assets (resources are shared and loaded once).
    static WorkerLoadOutcome worker_load(const std::shared_ptr<EngineState>& st,
                                         const std::string& path,
                                         bool assets_ok) {
        using namespace std::chrono_literals;
        magentart::detail::AutoreleasePool pool;
        const bool had_loaded_model = st->loaded.load(std::memory_order_acquire);
        auto fail = [&](RuntimeIssue issue, bool keep_current_model) {
            st->loading.store(false, std::memory_order_release);
            st->loading_model_candidate_valid.store(false, std::memory_order_release);
            st->runtime_issue.store(issue_value(issue), std::memory_order_release);
            st->load_failed.store(true, std::memory_order_release);
            if (!keep_current_model)
                st->loaded.store(false, std::memory_order_release);
            return keep_current_model ? WorkerLoadOutcome::preserved_current_model
                                      : WorkerLoadOutcome::failed;
        };
        st->loading.store(false, std::memory_order_release);
        st->loading_model_candidate_valid.store(false, std::memory_order_release);
        st->load_failed.store(false, std::memory_order_release);
        st->generation_failed.store(false, std::memory_order_release);
        st->runtime_issue.store(issue_value(RuntimeIssue::none), std::memory_order_release);
        if (!assets_ok) {
            magenta_v2_debug_log("load failed before model load: resources incomplete");
            return fail(RuntimeIssue::missing_resources, had_loaded_model);
        }
        if (path.empty() || !model_bundle_complete(path)) {
            magenta_v2_debug_log("load failed before model load: bundle incomplete for '" + path + "'");
            return fail(RuntimeIssue::missing_model_bundle, had_loaded_model);
        }
        if (!model_supported_for_realtime_host(path)) {
            magenta_v2_debug_log("load rejected before MLX: model unsupported on this Mac for '" +
                                 path + "'");
            return fail(RuntimeIssue::unsupported_model, had_loaded_model);
        }
        st->loading_model_candidate_valid.store(true, std::memory_order_release);
        st->loading.store(true, std::memory_order_release);

        bool loaded = false;
        try {
            // Do not call MLXEngine::unload() for model switches: it also destroys the
            // MusicCoCa/TFLite assets loaded by init_assets(), so the next prompt encode
            // fails even when the new model file itself loads. load_model() already
            // replaces the transformer state/function.
            magenta_v2_debug_log("loading model '" + path + "'");
            loaded = st->engine.load_model(path.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[PromptableAccompanistV2] MLX model load threw for '%s': %s\n",
                         path.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr,
                         "[PromptableAccompanistV2] MLX model load threw for '%s'.\n",
                         path.c_str());
        }
        if (!loaded) {
            magenta_v2_debug_log("MLXEngine::load_model returned false for '" + path + "'");
            return fail(RuntimeIssue::model_load_failed, false);
        }

        st->generated_frames.store(0, std::memory_order_relaxed);
        st->underrun_blocks.store(0, std::memory_order_relaxed);
        reset_generation_stagnation_watchdog(*st);

        std::string prompt;
        { std::lock_guard<std::mutex> lk(st->mutex_); prompt = st->current_prompt; }
        apply_prompt_to_engine(st->engine, prompt);
        st->applied_prompt_revision.store(st->prompt_revision.load(std::memory_order_acquire),
                                          std::memory_order_release);
        if (!wait_for_prompt_encoding_idle(st,
                                           "encoder startup for '" + path + "'",
                                           30s,
                                           true))
            return fail(RuntimeIssue::encoder_failed, false);
        if (st->engine.get_text_encoder_status() == 3 || st->engine.get_quantizer_status() == 3) {
            magenta_v2_debug_log("encoder failed after model load for '" + path + "'");
            return fail(RuntimeIssue::encoder_failed, false);
        }
        { std::lock_guard<std::mutex> lk(st->mutex_); st->loaded_model_path = path; }
        magenta_v2_debug_log("model load complete for '" + path + "'");
        st->loading.store(false, std::memory_order_release);
        st->loading_model_candidate_valid.store(false, std::memory_order_release);
        return WorkerLoadOutcome::loaded;
    }

    static bool generate_and_write_frame(const std::shared_ptr<EngineState>& st,
                                         float* L, float* R) {
        using namespace std::chrono_literals;
        namespace mc = magentart::core;
        {
            magentart::detail::AutoreleasePool pool;
            apply_realtime_inputs(st);
            const auto start = std::chrono::steady_clock::now();
            if (!st->engine.generate_frame(L, R)) {
                st->runtime_issue.store(issue_value(RuntimeIssue::generation_failed),
                                        std::memory_order_release);
                st->generation_failed.store(true, std::memory_order_release);
                st->load_failed.store(true, std::memory_order_release);
                return false;
            }
            const auto end = std::chrono::steady_clock::now();
            st->last_frame_ms.store(
                std::chrono::duration<float, std::milli>(end - start).count(),
                std::memory_order_relaxed);
            if (update_generation_stagnation_watchdog(*st, L, R, mc::kFrameSamples))
                recover_stagnant_generation(st);
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
            st->runtime_issue.store(issue_value(RuntimeIssue::generation_failed),
                                    std::memory_order_release);
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

    static bool init_assets(const std::shared_ptr<EngineState>& st) {
        magentart::detail::AutoreleasePool pool;
        bool ok = false;
        const std::string resources = default_resources();
        try {
            ok = st->engine.init_assets(resources.c_str(), "musiccoca");
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[PromptableAccompanistV2] MRT2 init_assets threw for '%s': %s\n",
                         resources.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr,
                         "[PromptableAccompanistV2] MRT2 init_assets threw for '%s'.\n",
                         resources.c_str());
        }
        if (!ok)
            std::fprintf(stderr, "[PromptableAccompanistV2] MRT2 shared resources not found at:\n"
                                 "    %s\n  The instrument stays silent until they are installed.\n",
                         resources.c_str());
        return ok;
    }

    static void worker_run(std::shared_ptr<EngineState> st) {
        using namespace std::chrono_literals;
        namespace mc = magentart::core;

        bool assets_ok = init_assets(st);

        // Initial load. Unlike before, a missing model is NOT fatal — the worker idles
        // and keeps servicing reload requests, so a model downloaded later from the
        // in-plugin Models overlay starts playing live, with no restart.
        const std::string initial = default_model();
        magenta_v2_debug_log("initial model resolved to '" + initial + "'");
        std::error_code ec;
        if (assets_ok && !std::filesystem::exists(initial, ec))
            std::fprintf(stderr,
                "[PromptableAccompanistV2] No MRT2 model installed yet. Open Settings -> Models to\n"
                "  download one (mrt2_small or mrt2_base) — it will start playing without a restart.\n");
        float L[mc::kFrameSamples], R[mc::kFrameSamples];
        bool ok = worker_load(st, initial, assets_ok) == WorkerLoadOutcome::loaded;
        if (ok && st->running.load(std::memory_order_acquire))
            ok = prime_output_ring(st, L, R);
        st->loaded.store(ok, std::memory_order_release);
        st->load_failed.store(!ok, std::memory_order_release);

        while (st->running.load()) {
            // Service a hot-reload at a frame boundary (on this MLX-owning thread).
            if (st->reload_requested.exchange(false, std::memory_order_acquire)) {
                std::string path;
                std::string loaded_path;
                {
                    std::lock_guard<std::mutex> lk(st->mutex_);
                    path = st->pending_model_path;
                    loaded_path = st->loaded_model_path;
                }
                if (st->loaded.load(std::memory_order_acquire) && !path.empty() &&
                    path == loaded_path) {
                    magenta_v2_debug_log("reload skipped; requested model is already loaded: '" +
                                         path + "'");
                    st->loading.store(false, std::memory_order_release);
                    st->loading_model_candidate_valid.store(false, std::memory_order_release);
                    st->load_failed.store(false, std::memory_order_release);
                    st->generation_failed.store(false, std::memory_order_release);
                    st->runtime_issue.store(issue_value(RuntimeIssue::none),
                                            std::memory_order_release);
                    continue;
                }
                if (!assets_ok) assets_ok = init_assets(st);
                const WorkerLoadOutcome load_outcome = worker_load(st, path, assets_ok);
                if (load_outcome == WorkerLoadOutcome::loaded &&
                    prime_output_ring(st, L, R)) {
                    st->loaded.store(true, std::memory_order_release);
                    st->load_failed.store(false, std::memory_order_release);
                } else if (load_outcome == WorkerLoadOutcome::loaded) {
                    st->loaded.store(false, std::memory_order_release);
                    st->runtime_issue.store(issue_value(RuntimeIssue::generation_failed),
                                            std::memory_order_release);
                    st->generation_failed.store(true, std::memory_order_release);
                    st->load_failed.store(true, std::memory_order_release);
                }
            }
            if (!st->loaded.load()) {               // no model yet → idle, stay responsive
                std::this_thread::sleep_for(20ms);
                continue;
            }
            if (!generate_and_write_frame(st, L, R)) {
                if (!st->running.load(std::memory_order_relaxed)) break;
                st->loaded.store(false, std::memory_order_release);
                std::fprintf(stderr,
                             "[PromptableAccompanistV2] MRT2 generation stopped; "
                             "the instrument will stay silent until the model is reloaded.\n");
            }
        }
        wait_for_prompt_encoding_idle_before_teardown(st);
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
