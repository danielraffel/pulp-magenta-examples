// Headless smoke test for the Promptable Accompanist V2 processor. Validates the
// instrument contract without loading the model, then exercises the freeze/loop helper
// against synthetic audio so the sampler primitives compile and publish a held sample.
#include "accompanist.hpp"
#include "freeze_loop_sampler.hpp"

#include <pulp/format/clap_entry.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widgets.hpp>

#include <dlfcn.h>

#if defined(__APPLE__) && defined(PROMPTABLE_ACCOMPANIST_V2_HAS_AUSDK)
#include <pulp/format/au_v2_instrument.hpp>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::examples::accompanist_v2;

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); return 1; } } while (0)

namespace {

pulp::format::Processor* g_last_adapter_processor = nullptr;

std::unique_ptr<pulp::format::Processor> create_v2_adapter_test_processor() {
    auto processor = create_promptable_accompanist_v2();
    g_last_adapter_processor = processor.get();
    return processor;
}

Processor* last_v2_adapter_processor() {
    return dynamic_cast<Processor*>(g_last_adapter_processor);
}

void configure_adapter_test_factory() {
    g_last_adapter_processor = nullptr;
    pulp::format::register_plugin(create_v2_adapter_test_processor);
    pulp::format::clap_generic::g_factory = create_v2_adapter_test_processor;
    pulp::format::clap_generic::init_descriptor();
}

class EnvGuard {
public:
    EnvGuard(const char* key, const char* value) : key_(key) {
        if (const char* old = std::getenv(key_)) {
            had_old_ = true;
            old_ = old;
        }
        if (value) setenv(key_, value, 1);
        else unsetenv(key_);
    }

    ~EnvGuard() {
        if (had_old_) setenv(key_, old_.c_str(), 1);
        else unsetenv(key_);
    }

private:
    const char* key_;
    bool had_old_ = false;
    std::string old_;
};

std::filesystem::path unique_temp_dir(const char* name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                (std::string(name) + "-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

void touch_file(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << "x";
}

void touch_model_file(const std::filesystem::path& path) {
    touch_file(path);
    const auto expected = magenta_demo::expected_magenta_model_file_size(path);
    if (expected > 0) std::filesystem::resize_file(path, expected);
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << text;
}

void write_installed_model_metadata(const std::filesystem::path& pulp_home,
                                    const std::string& model_id,
                                    const std::filesystem::path& checkpoint) {
    write_text_file(pulp_home / "magenta/models" / (model_id + ".json"),
                    "{\n"
                    "  \"model_id\": \"" + model_id + "\",\n"
                    "  \"backend\": \"mlx\",\n"
                    "  \"checkpoint_ref\": \"hf://google/magenta-realtime-2/models/" +
                        model_id + "/" + model_id + ".mlxfn\",\n"
                    "  \"resolved_checkpoint_path\": \"" + checkpoint.string() + "\"\n"
                    "}\n");
}

void write_active_model_state(const std::filesystem::path& pulp_home,
                              const std::string& model_id,
                              const std::filesystem::path& checkpoint) {
    write_text_file(pulp_home / "magenta/model-state.json",
                    "{\n"
                    "  \"active_model_id\": \"" + model_id + "\",\n"
                    "  \"configured_model_id\": \"" + model_id + "\",\n"
                    "  \"resolved_checkpoint_path\": \"" + checkpoint.string() + "\"\n"
                    "}\n");
}

void fill_test_audio(pulp::audio::Buffer<float>& block, int block_index) {
    constexpr double kTwoPi = 6.28318530717958647692;
    for (std::size_t ch = 0; ch < block.num_channels(); ++ch) {
        auto samples = block.channel(ch);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto phase = (static_cast<double>(block_index * samples.size() + i) / 64.0) +
                               static_cast<double>(ch) * 0.125;
            samples[i] = static_cast<float>(0.25 * std::sin(kTwoPi * phase));
        }
    }
}

void fill_constant_audio(pulp::audio::Buffer<float>& block, float value) {
    for (std::size_t ch = 0; ch < block.num_channels(); ++ch) {
        auto samples = block.channel(ch);
        for (auto& sample : samples) sample = value;
    }
}

void fill_keyed_sampler_test_audio(pulp::audio::Buffer<float>& block, int block_index) {
    constexpr double kTwoPi = 6.28318530717958647692;
    for (std::size_t ch = 0; ch < block.num_channels(); ++ch) {
        auto samples = block.channel(ch);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto n = static_cast<double>(block_index * samples.size() + i) +
                           static_cast<double>(ch) * 23.0;
            const auto phase = 0.0065 * n + 0.0000017 * n * n;
            samples[i] = static_cast<float>(0.22 * std::sin(kTwoPi * phase));
        }
    }
}

double max_abs_error_from(const pulp::audio::Buffer<float>& block, double expected) {
    double max_error = 0.0;
    for (std::size_t ch = 0; ch < block.num_channels(); ++ch) {
        for (const auto sample : block.channel(ch)) {
            const auto error = std::fabs(static_cast<double>(sample) - expected);
            if (error > max_error) max_error = error;
        }
    }
    return max_error;
}

double rms_audio(const pulp::audio::Buffer<float>& block) {
    double sum_squares = 0.0;
    std::size_t count = 0;
    for (std::size_t ch = 0; ch < block.num_channels(); ++ch) {
        for (const auto sample : block.channel(ch)) {
            const auto value = static_cast<double>(sample);
            sum_squares += value * value;
            ++count;
        }
    }
    return count > 0 ? std::sqrt(sum_squares / static_cast<double>(count)) : 0.0;
}

bool tree_contains_label_text(const pulp::view::View& view, const std::string& needle) {
    if (const auto* label = dynamic_cast<const pulp::view::Label*>(&view);
        label && label->text().find(needle) != std::string::npos)
        return true;
    for (std::size_t i = 0; i < view.child_count(); ++i) {
        if (const auto* child = view.child_at(i))
            if (tree_contains_label_text(*child, needle)) return true;
    }
    return false;
}

double process_constant_blocks_and_measure_error(FreezeLoopSampler& sampler,
                                                 pulp::audio::Buffer<float>& block,
                                                 FreezeLoopSamplerControls controls,
                                                 float live_value,
                                                 double expected_output,
                                                 int settle_blocks,
                                                 int measure_blocks) {
    for (int i = 0; i < settle_blocks; ++i) {
        fill_constant_audio(block, live_value);
        auto view = block.view();
        sampler.process(view, controls);
    }
    double max_error = 0.0;
    for (int i = 0; i < measure_blocks; ++i) {
        fill_constant_audio(block, live_value);
        auto view = block.view();
        sampler.process(view, controls);
        const auto block_error = max_abs_error_from(block, expected_output);
        if (block_error > max_error) max_error = block_error;
    }
    return max_error;
}

bool wait_for_frozen_sample(FreezeLoopSampler& sampler,
                            pulp::audio::Buffer<float>& block,
                            FreezeLoopSamplerControls controls) {
    using namespace std::chrono_literals;
    for (int i = 0; i < 250; ++i) {
        fill_test_audio(block, 100 + i);
        auto view = block.view();
        sampler.process(view, controls);
        const auto status = sampler.status();
        if (status.frozen && status.sample_frames > 0 && status.captures_completed > 0) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool render_keyed_frozen_note(int note, std::vector<float>& result) {
    FreezeLoopSampler sampler;
    FreezeLoopSamplerConfig config;
    config.num_channels = 2;
    config.sample_rate = 48000.0;
    config.max_block_frames = 64;
    config.max_capture_seconds = 0.25;
    config.sample_slots = 2;
    if (!sampler.prepare(config)) return false;

    pulp::audio::Buffer<float> block(2, 64);
    FreezeLoopSamplerControls live_controls;
    live_controls.freeze = false;
    live_controls.capture_seconds = 0.05;
    live_controls.loop_crossfade_ms = 5.0;

    for (int i = 0; i < 32; ++i) {
        fill_keyed_sampler_test_audio(block, i);
        auto view = block.view();
        sampler.process(view, live_controls);
    }

    FreezeLoopSamplerControls freeze_controls = live_controls;
    freeze_controls.freeze = true;
    fill_keyed_sampler_test_audio(block, 32);
    auto freeze_view = block.view();
    sampler.process(freeze_view, freeze_controls);
    if (!wait_for_frozen_sample(sampler, block, freeze_controls)) {
        sampler.shutdown();
        return false;
    }

    for (int i = 0; i < 32; ++i) {
        fill_constant_audio(block, 0.0f);
        auto view = block.view();
        sampler.process(view, freeze_controls);
    }

    pulp::midi::MidiBuffer note_on;
    note_on.add(pulp::midi::MidiEvent::note_on(0, static_cast<std::uint8_t>(note), 110));
    FreezeLoopSamplerControls keyed_controls = freeze_controls;
    keyed_controls.midi = &note_on;
    keyed_controls.root_note = 60;
    fill_constant_audio(block, 0.0f);
    auto note_on_view = block.view();
    sampler.process(note_on_view, keyed_controls);

    pulp::midi::MidiBuffer no_midi;
    keyed_controls.midi = &no_midi;
    result.clear();
    for (int i = 0; i < 12; ++i) {
        fill_constant_audio(block, 0.0f);
        auto view = block.view();
        sampler.process(view, keyed_controls);
        if (i >= 4) {
            const auto channel = block.channel(0);
            result.insert(result.end(), channel.begin(), channel.end());
        }
    }

    sampler.shutdown();
    return !result.empty();
}

void process_runtime_block(Processor& processor,
                           pulp::audio::Buffer<float>& output,
                           std::uint64_t block_index) {
    output.clear();
    auto output_view = output.view();
    pulp::audio::BufferView<const float> input_view;
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::format::ProcessContext context;
    context.sample_rate = 48000.0;
    context.num_samples = static_cast<int>(output.num_samples());
    context.is_playing = true;
    context.tempo_bpm = 120.0;
    context.position_samples = static_cast<std::int64_t>(block_index * output.num_samples());
    context.position_beats = static_cast<double>(context.position_samples) *
                             context.tempo_bpm / (context.sample_rate * 60.0);

    pulp::runtime::ScopedNoAlloc no_alloc;
    processor.process(output_view, input_view, midi_in, midi_out, context);
}

void sleep_for_runtime_block(const pulp::audio::Buffer<float>& output) {
    const auto seconds = static_cast<double>(output.num_samples()) / 48000.0;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

bool wait_for_generated_audio(Processor& processor,
                              pulp::audio::Buffer<float>& output,
                              std::uint64_t& block_index,
                              double min_rms,
                              int max_blocks) {
    using namespace std::chrono_literals;
    for (int i = 0; i < max_blocks; ++i) {
        process_runtime_block(processor, output, block_index++);
        if (rms_audio(output) >= min_rms) return true;
        sleep_for_runtime_block(output);
    }
    return false;
}

bool wait_for_frozen_runtime_sample(Processor& processor,
                                    pulp::audio::Buffer<float>& output,
                                    std::uint64_t& block_index,
                                    std::uint64_t required_captures,
                                    double min_rms,
                                    int max_blocks) {
    using namespace std::chrono_literals;
    for (int i = 0; i < max_blocks; ++i) {
        process_runtime_block(processor, output, block_index++);
        const auto status = processor.freeze_sampler_status();
        if (status.frozen && status.sample_frames > 0 &&
            status.captures_completed >= required_captures &&
            rms_audio(output) >= min_rms) {
            return true;
        }
        sleep_for_runtime_block(output);
    }
    return false;
}

bool wait_for_released_runtime_sample(Processor& processor,
                                      pulp::audio::Buffer<float>& output,
                                      std::uint64_t& block_index,
                                      int max_blocks) {
    using namespace std::chrono_literals;
    for (int i = 0; i < max_blocks; ++i) {
        process_runtime_block(processor, output, block_index++);
        const auto status = processor.freeze_sampler_status();
        if (!status.frozen && !status.materialize_pending && !status.hold_active) {
            return true;
        }
        sleep_for_runtime_block(output);
    }
    return false;
}

int check_model_registry_defaults() {
    const auto& models = magenta_demo::magenta_models();
    CHECK(models.size() == 2, "two MRT2 model choices are registered");
    CHECK(models[0].model_id == "mrt2_small", "small model is listed first");
    CHECK(models[0].is_recommended, "small model is recommended");
    CHECK(models[1].model_id == "mrt2_base", "large model is listed second");
    CHECK(!models[1].is_recommended, "large model is not the default recommendation");

    const auto home = unique_temp_dir("pulp-magenta-v2-model-default");
    EnvGuard home_guard("HOME", home.c_str());
    EnvGuard pulp_home_guard("PULP_HOME", nullptr);
    EnvGuard explicit_model_guard("MRT2_MODEL", nullptr);
    EnvGuard hardware_guard("PULP_MAGENTA_V2_HW_MODEL", "Mac16,1");
    EnvGuard unsupported_guard("PULP_MAGENTA_V2_ALLOW_UNSUPPORTED_MODEL", nullptr);

    const auto root = home / "Documents/Magenta/magenta-rt-v2/models";
    const auto small = root / "mrt2_small/mrt2_small.mlxfn";
    const auto base = root / "mrt2_base/mrt2_base.mlxfn";
    touch_model_file(base);
    touch_model_file(root / "mrt2_base/mrt2_base_state.safetensors");
    touch_model_file(small);
    touch_model_file(root / "mrt2_small/mrt2_small_state.safetensors");
    CHECK(default_model() == small.string(), "legacy resolver prefers small when both models exist");

    std::filesystem::remove(small);
    CHECK(default_model() == base.string(), "legacy resolver falls back to base when small is absent");

    touch_model_file(small);
    {
        EnvGuard m1_guard("PULP_MAGENTA_V2_HW_MODEL", "MacBookPro18,4");
        CHECK(default_model() == small.string(),
              "M1 resolver still uses a complete legacy small model");
        std::filesystem::remove(small);
        CHECK(default_model().empty(),
              "M1 resolver refuses legacy base when small is absent");
    }
    std::filesystem::remove_all(home);
    return 0;
}

int check_active_model_requires_complete_bundle() {
    const auto home = unique_temp_dir("pulp-magenta-v2-active-bundle");
    const auto legacy_home = unique_temp_dir("pulp-magenta-v2-legacy-bundle");
    EnvGuard home_guard("HOME", legacy_home.c_str());
    EnvGuard pulp_home_guard("PULP_HOME", home.c_str());
    EnvGuard explicit_model_guard("MRT2_MODEL", nullptr);
    EnvGuard hardware_guard("PULP_MAGENTA_V2_HW_MODEL", "Mac16,1");
    EnvGuard unsupported_guard("PULP_MAGENTA_V2_ALLOW_UNSUPPORTED_MODEL", nullptr);

    const auto shared = home / "magenta/models/mrt2_small/mrt2_small.mlxfn";
    touch_model_file(shared);
    write_installed_model_metadata(home, "mrt2_small", shared);
    write_active_model_state(home, "mrt2_small", shared);

    const auto legacy_root = legacy_home / "Documents/Magenta/magenta-rt-v2/models";
    const auto legacy_small = legacy_root / "mrt2_small/mrt2_small.mlxfn";
    touch_model_file(legacy_small);
    touch_model_file(legacy_root / "mrt2_small/mrt2_small_state.safetensors");
    CHECK(default_model() == legacy_small.string(),
          "incomplete active shared model falls back to a complete compatible local model");

    touch_model_file(home / "magenta/models/mrt2_small/mrt2_small_state.safetensors");
    CHECK(default_model() == shared.string(),
          "complete active shared model resolves to the shared checkpoint");

    std::filesystem::remove_all(home);
    std::filesystem::remove_all(legacy_home);
    return 0;
}

int check_m1_resolver_avoids_large_model() {
    const auto home = unique_temp_dir("pulp-magenta-v2-m1-large");
    const auto legacy_home = unique_temp_dir("pulp-magenta-v2-m1-large-legacy");
    EnvGuard home_guard("HOME", legacy_home.c_str());
    EnvGuard pulp_home_guard("PULP_HOME", home.c_str());
    EnvGuard explicit_model_guard("MRT2_MODEL", nullptr);
    EnvGuard hardware_guard("PULP_MAGENTA_V2_HW_MODEL", "MacBookPro18,4");
    EnvGuard unsupported_guard("PULP_MAGENTA_V2_ALLOW_UNSUPPORTED_MODEL", nullptr);

    const auto shared_base = home / "magenta/models/mrt2_base/mrt2_base.mlxfn";
    touch_model_file(shared_base);
    touch_model_file(home / "magenta/models/mrt2_base/mrt2_base_state.safetensors");
    write_installed_model_metadata(home, "mrt2_base", shared_base);
    write_active_model_state(home, "mrt2_base", shared_base);
    CHECK(default_model().empty(),
          "M1 resolver refuses active shared large model when no small fallback exists");

    const auto shared_small = home / "magenta/models/mrt2_small/mrt2_small.mlxfn";
    touch_model_file(shared_small);
    touch_model_file(home / "magenta/models/mrt2_small/mrt2_small_state.safetensors");
    write_installed_model_metadata(home, "mrt2_small", shared_small);
    CHECK(default_model() == shared_small.string(),
          "M1 resolver falls back from active large to shared small");

    {
        EnvGuard non_m1_guard("PULP_MAGENTA_V2_HW_MODEL", "Mac16,1");
        CHECK(default_model() == shared_base.string(),
              "non-M1 resolver preserves an active complete large model");
    }

    std::filesystem::remove_all(home);
    std::filesystem::remove_all(legacy_home);
    return 0;
}

int check_runtime_status_priority() {
    Processor processor;
    processor.force_runtime_flags_for_test(true,
                                           true,
                                           false,
                                           RuntimeIssue::none,
                                           4);
    CHECK(processor.runtime_status_text().empty(),
          "generated audio state clears stale loading status");

    processor.force_runtime_flags_for_test(false,
                                           true,
                                           true,
                                           RuntimeIssue::missing_model_bundle,
                                           0);
    CHECK(processor.runtime_status_text().find("download a model") != std::string::npos,
          "missing local model status tells the user to download a model");

    processor.force_runtime_flags_for_test(true,
                                           false,
                                           true,
                                           RuntimeIssue::missing_model_bundle,
                                           4);
    CHECK(processor.runtime_status_text().find("download or repair a model") != std::string::npos,
          "deleted loaded model status tells the user to download or repair a model");

    processor.force_runtime_flags_for_test(true,
                                           false,
                                           true,
                                           RuntimeIssue::missing_resources,
                                           4);
    CHECK(processor.runtime_status_text().empty(),
          "loaded model suppresses stale resource repair warnings");

    processor.force_runtime_flags_for_test(false,
                                           true,
                                           false,
                                           RuntimeIssue::none,
                                           0);
    CHECK(processor.runtime_status_text().find("download a model") != std::string::npos,
          "candidate-less loading state tells the user to download a model");
    CHECK(!processor.model_ready_for_editor_for_test(),
          "candidate-less loading state stays on the download-model gate");

    processor.force_runtime_flags_for_test(false,
                                           true,
                                           false,
                                           RuntimeIssue::none,
                                           0,
                                           true);
    CHECK(processor.runtime_status_text().find("Loading Magenta model") != std::string::npos,
          "valid candidate loading state still reports model loading");
    CHECK(processor.model_ready_for_editor_for_test(),
          "valid candidate loading state can show the instrument editor");

    processor.force_runtime_flags_for_test(false,
                                           true,
                                           true,
                                           RuntimeIssue::unsupported_model,
                                           0);
    CHECK(processor.runtime_status_text().find("Small model") != std::string::npos,
          "unsupported real-time model status points users to Small");
    return 0;
}

int check_worker_preflight_rejections_do_not_publish_loading() {
    const auto home = unique_temp_dir("pulp-magenta-v2-preflight-status");

    {
        Processor processor;
        auto st = std::make_shared<EngineState>();
        processor.set_engine_state_for_test(st);
        const auto missing = home / "mrt2_small/mrt2_small.mlxfn";
        const auto outcome = Processor::worker_load_for_test(st, missing.string(), true);
        CHECK(outcome == WorkerLoadOutcome::failed,
              "missing model bundle is rejected by the worker preflight path");
        CHECK(!st->loading.load(std::memory_order_acquire),
              "missing model bundle does not publish loading");
        CHECK(!st->loading_model_candidate_valid.load(std::memory_order_acquire),
              "missing model bundle does not publish a valid loading candidate");
        CHECK(processor.runtime_status_text().find("download a model") != std::string::npos,
              "missing model worker rejection tells the user to download a model");
    }

    {
        Processor processor;
        auto st = std::make_shared<EngineState>();
        processor.set_engine_state_for_test(st);
        const auto small = home / "mrt2_small/mrt2_small.mlxfn";
        touch_model_file(small);
        touch_model_file(home / "mrt2_small/mrt2_small_state.safetensors");
        const auto outcome = Processor::worker_load_for_test(st, small.string(), false);
        CHECK(outcome == WorkerLoadOutcome::failed,
              "missing resources are rejected by the worker preflight path");
        CHECK(!st->loading.load(std::memory_order_acquire),
              "missing resources do not publish loading");
        CHECK(processor.runtime_status_text().find("incomplete") != std::string::npos,
              "missing resources worker rejection points to model repair");
    }

    {
        EnvGuard hardware_guard("PULP_MAGENTA_V2_HW_MODEL", "MacBookPro18,4");
        EnvGuard unsupported_guard("PULP_MAGENTA_V2_ALLOW_UNSUPPORTED_MODEL", nullptr);
        Processor processor;
        auto st = std::make_shared<EngineState>();
        processor.set_engine_state_for_test(st);
        const auto base = home / "mrt2_base/mrt2_base.mlxfn";
        touch_model_file(base);
        touch_model_file(home / "mrt2_base/mrt2_base_state.safetensors");
        const auto outcome = Processor::worker_load_for_test(st, base.string(), true);
        CHECK(outcome == WorkerLoadOutcome::failed,
              "unsupported M1-family model is rejected by the worker preflight path");
        CHECK(!st->loading.load(std::memory_order_acquire),
              "unsupported M1-family model does not publish loading");
        CHECK(processor.runtime_status_text().find("Small model") != std::string::npos,
              "unsupported M1-family model points users to Small");
    }

    std::filesystem::remove_all(home);
    return 0;
}

int check_status_banner_refreshes_after_late_frame_clock() {
    bool model_ready = true;
    std::string runtime_status = "Loading Magenta model...";

    auto root = std::make_unique<magenta_demo::AccompanistRoot>(
        [](std::uint32_t, float) {},
        [](std::uint32_t) { return 0.5f; },
        [](std::uint32_t) { return std::string("0"); },
        [](const std::string&) {},
        [](pulp::view::View&, pulp::view::Point) { return false; },
        [&model_ready] { return model_ready; },
        [&runtime_status] { return runtime_status; },
        [] {},
        "warm analog pads");
    auto* root_ptr = root.get();
    CHECK(root_ptr->child_count() == 2,
          "transient loading status is not rendered once the editor is usable");

    pulp::view::View parent;
    parent.add_child(std::move(root));

    pulp::view::FrameClock clock;
    parent.set_frame_clock(&clock);
    root_ptr->layout_children();
    CHECK(root_ptr->child_count() == 2,
          "late frame-clock attachment does not need to clear transient loading UI");

    runtime_status = "Generated audio is underrunning. Try Small, 48 kHz, and close other GPU-heavy apps.";
    clock.tick(1.0f / 60.0f);
    CHECK(root_ptr->child_count() == 3,
          "runtime status banner appears when a later warning is published");

    runtime_status.clear();
    clock.tick(1.0f / 60.0f);
    CHECK(root_ptr->child_count() == 2,
          "runtime status banner clears when the later warning resolves");
    return 0;
}

int check_model_section_surfaces_storage_locations() {
    const auto home = unique_temp_dir("pulp-magenta-v2-model-section");
    const auto pulp_home = home / ".pulp";
    const auto home_string = home.string();
    const auto pulp_home_string = pulp_home.string();
    EnvGuard home_guard("HOME", home_string.c_str());
    EnvGuard pulp_home_guard("PULP_HOME", pulp_home_string.c_str());

    const auto shared_root = pulp_home / "magenta/models";
    touch_model_file(shared_root / "mrt2_small/mrt2_small.mlxfn");
    touch_model_file(shared_root / "mrt2_small/mrt2_small_state.safetensors");

    magenta_demo::ModelSection section([] {});
    CHECK(tree_contains_label_text(section, "Model folder"),
          "model section shows the shared model folder");
    CHECK(tree_contains_label_text(section, shared_root.string()),
          "model section shows the resolved shared Pulp model path");
    CHECK(tree_contains_label_text(section, "Small detected"),
          "model section reports a complete shared Small bundle");
    return 0;
}

int check_prompt_clear_contract() {
    CHECK(prompt_has_text_conditioning("warm analog pads"),
          "non-empty prompt uses text conditioning");
    CHECK(!prompt_has_text_conditioning(""),
          "empty prompt disables text conditioning");
    CHECK(!prompt_has_text_conditioning(" \n\t"),
          "whitespace-only prompt disables text conditioning");
    const auto changed_at = std::chrono::steady_clock::time_point{} +
                            std::chrono::milliseconds(1000);
    CHECK(!prompt_change_is_settled(changed_at, changed_at + std::chrono::milliseconds(250)),
          "prompt edits debounce transient empty replacement states");
    CHECK(prompt_change_is_settled(changed_at, changed_at + kPromptApplyDebounce),
          "prompt edits apply after the debounce window");

    auto view = make_accompanist_native_view(
        [](std::uint32_t, float) {},
        [](std::uint32_t) { return 0.5f; },
        [](std::uint32_t) { return std::string("0"); },
        [](const std::string&) {},
        [](pulp::view::View&, pulp::view::Point) { return false; },
        "");
    CHECK(view->child_count() >= 3, "native editor renders a prompt field");
    auto* prompt_box = dynamic_cast<pulp::view::TextEditor*>(view->child_at(2));
    CHECK(prompt_box != nullptr, "prompt field is a TextEditor");
    CHECK(prompt_box->text().empty(),
          "empty prompt remains empty instead of restoring the startup default");
    CHECK(prompt_box->placeholder == "describe the music...",
          "empty prompt is represented by placeholder text");
    return 0;
}

int check_generation_watchdog_detects_stagnant_frames() {
    EngineState st;
    std::array<float, 256> left{};
    std::array<float, 256> right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 0.08f + static_cast<float>(i % 11) * 0.001f;
        right[i] = -0.04f + static_cast<float>(i % 7) * 0.001f;
    }

    CHECK(!update_generation_stagnation_watchdog(st, left.data(), right.data(), left.size()),
          "generation watchdog accepts the first audible frame");
    for (int i = 1; i < kGenerationStagnationFrameLimit; ++i) {
        CHECK(!update_generation_stagnation_watchdog(st, left.data(), right.data(), left.size()),
              "generation watchdog waits through tolerated repeated frames");
    }
    CHECK(update_generation_stagnation_watchdog(st, left.data(), right.data(), left.size()),
          "generation watchdog detects repeated generated frames");
    CHECK(st.previous_generation_signature == 0 && st.repeated_generation_frames == 0,
          "generation watchdog resets after reporting stagnation");

    left[0] += 0.25f;
    CHECK(!update_generation_stagnation_watchdog(st, left.data(), right.data(), left.size()),
          "generation watchdog resets on changed audio");
    left.fill(0.0f);
    right.fill(0.0f);
    CHECK(!update_generation_stagnation_watchdog(st, left.data(), right.data(), left.size()),
          "generation watchdog ignores silence");
    return 0;
}

int check_package_audit_mutes_generated_audio_by_default() {
    {
        EnvGuard audit_guard("PULP_STANDALONE_PACKAGE_AUDIT", "1");
        EnvGuard generic_audio_guard("PULP_STANDALONE_PACKAGE_AUDIT_AUDIO", nullptr);
        EnvGuard app_audio_guard("PULP_MAGENTA_V2_PACKAGE_AUDIT_AUDIO", nullptr);
        CHECK(package_audit_audio_muted(),
              "package audit mutes generated audio by default");
    }
    {
        EnvGuard audit_guard("PULP_STANDALONE_PACKAGE_AUDIT", "1");
        EnvGuard generic_audio_guard("PULP_STANDALONE_PACKAGE_AUDIT_AUDIO", "1");
        EnvGuard app_audio_guard("PULP_MAGENTA_V2_PACKAGE_AUDIT_AUDIO", nullptr);
        CHECK(!package_audit_audio_muted(),
              "generic opt-in allows audible package audit");
    }
    {
        EnvGuard audit_guard("PULP_STANDALONE_PACKAGE_AUDIT", "1");
        EnvGuard generic_audio_guard("PULP_STANDALONE_PACKAGE_AUDIT_AUDIO", nullptr);
        EnvGuard app_audio_guard("PULP_MAGENTA_V2_PACKAGE_AUDIT_AUDIO", "1");
        CHECK(!package_audit_audio_muted(),
              "app opt-in allows audible package audit");
    }
    {
        EnvGuard audit_guard("PULP_STANDALONE_PACKAGE_AUDIT", nullptr);
        EnvGuard generic_audio_guard("PULP_STANDALONE_PACKAGE_AUDIT_AUDIO", nullptr);
        EnvGuard app_audio_guard("PULP_MAGENTA_V2_PACKAGE_AUDIT_AUDIO", nullptr);
        CHECK(!package_audit_audio_muted(),
              "normal launches are not package-audit muted");
    }
    return 0;
}

int check_frozen_loop_state_round_trip() {
    FreezeLoopSamplerSnapshot snapshot;
    snapshot.num_channels = 2;
    snapshot.num_frames = 128;
    snapshot.sample_rate = 48000.0;
    snapshot.loop_crossfade_ms = 0.0;
    snapshot.planar_samples.assign(static_cast<std::size_t>(snapshot.num_channels *
                                                            snapshot.num_frames),
                                   0.25f);

    const auto blob = serialize_frozen_loop_snapshot(snapshot);
    CHECK(!blob.empty(), "frozen loop snapshot serializes");
    const auto decoded = deserialize_frozen_loop_snapshot(blob);
    CHECK(decoded.has_value(), "frozen loop snapshot deserializes");
    CHECK(decoded->num_channels == snapshot.num_channels,
          "frozen loop snapshot preserves channel count");
    CHECK(decoded->num_frames == snapshot.num_frames,
          "frozen loop snapshot preserves frame count");
    CHECK(decoded->planar_samples == snapshot.planar_samples,
          "frozen loop snapshot preserves audio samples");

    FreezeLoopSampler sampler;
    FreezeLoopSamplerConfig config;
    config.num_channels = 2;
    config.sample_rate = 48000.0;
    config.max_block_frames = 64;
    config.max_capture_seconds = 0.25;
    config.sample_slots = 2;
    CHECK(sampler.prepare(config), "snapshot restore sampler prepares");
    CHECK(sampler.restore_frozen_snapshot(snapshot),
          "snapshot restore publishes a frozen loop");

    pulp::audio::Buffer<float> block(2, 64);
    FreezeLoopSamplerControls controls;
    controls.freeze = true;
    controls.capture_seconds = 0.05;
    controls.loop_crossfade_ms = 0.0;
    const auto restored_error = process_constant_blocks_and_measure_error(sampler,
                                                                          block,
                                                                          controls,
                                                                          0.0f,
                                                                          0.25,
                                                                          0,
                                                                          4);
    CHECK(restored_error < 0.001,
          "snapshot-restored sampler renders the persisted loop");
    sampler.shutdown();

    const auto home = unique_temp_dir("pulp-magenta-v2-frozen-state");
    const auto pulp_home = home / ".pulp";
    const auto home_string = home.string();
    const auto pulp_home_string = pulp_home.string();
    EnvGuard home_guard("HOME", home_string.c_str());
    EnvGuard pulp_home_guard("PULP_HOME", pulp_home_string.c_str());
    EnvGuard explicit_model_guard("MRT2_MODEL", nullptr);
    EnvGuard worker_guard("PULP_MAGENTA_V2_TEST_DISABLE_WORKER", "1");

    Processor processor;
    pulp::state::StateStore state;
    processor.set_state_store(&state);
    processor.define_parameters(state);
    state.set_value(kFreeze, 1.0f);
    state.set_value(kVolumeDb, 0.0f);
    CHECK(processor.deserialize_plugin_state(blob),
          "processor accepts a frozen loop payload before prepare");

    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 64;
    prepare.input_channels = 0;
    prepare.output_channels = 2;
    processor.prepare(prepare);

    pulp::audio::Buffer<float> output(2, 64);
    std::uint64_t block_index = 0;
    process_runtime_block(processor, output, block_index++);
    CHECK(processor.freeze_sampler_status().frozen,
          "processor restores frozen sampler state during prepare");
    CHECK(max_abs_error_from(output, 0.25) < 0.001,
          "processor renders restored frozen audio after state recall");
    CHECK(!processor.serialize_plugin_state().empty(),
          "processor reserializes restored frozen audio while Freeze is on");
    const auto exported_path = processor.export_active_frozen_loop_wav_for_drag();
    CHECK(!exported_path.empty(),
          "processor exports active frozen audio to a draggable WAV");
    auto exported_audio = pulp::audio::read_audio_file(exported_path);
    CHECK(exported_audio.has_value(),
          "exported frozen-loop WAV reads back");
    CHECK(exported_audio->num_channels() == snapshot.num_channels,
          "exported frozen-loop WAV preserves channel count");
    CHECK(exported_audio->num_frames() == snapshot.num_frames,
          "exported frozen-loop WAV preserves frame count");
    CHECK(exported_audio->sample_rate == static_cast<std::uint32_t>(snapshot.sample_rate),
          "exported frozen-loop WAV preserves sample rate");
    CHECK(!exported_audio->channels.empty() &&
          !exported_audio->channels[0].empty() &&
          std::fabs(exported_audio->channels[0][0] - 0.25f) < 0.01f,
          "exported frozen-loop WAV preserves sample content");
    std::filesystem::remove(exported_path);
    state.set_value(kFreeze, 0.0f);
    CHECK(processor.serialize_plugin_state().empty(),
          "processor omits frozen audio payload when Freeze is off");
    CHECK(processor.export_active_frozen_loop_wav_for_drag().empty(),
          "processor does not export inactive frozen audio after Freeze is released");
    processor.release();
    std::filesystem::remove_all(home);
    return 0;
}

std::vector<std::uint8_t> make_adapter_test_frozen_loop_blob(float sample_value) {
    FreezeLoopSamplerSnapshot snapshot;
    snapshot.num_channels = 2;
    snapshot.num_frames = 128;
    snapshot.sample_rate = 48000.0;
    snapshot.loop_crossfade_ms = 0.0;
    snapshot.planar_samples.assign(static_cast<std::size_t>(snapshot.num_channels *
                                                            snapshot.num_frames),
                                   sample_value);
    return serialize_frozen_loop_snapshot(snapshot);
}

void prepare_v2_processor_for_adapter_recall(Processor& processor) {
    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 64;
    prepare.input_channels = 0;
    prepare.output_channels = 2;
    processor.prepare(prepare);
}

bool adapter_recall_renders_frozen_loop(Processor& processor, float expected) {
    pulp::audio::Buffer<float> output(2, 64);
    std::uint64_t block_index = 0;
    process_runtime_block(processor, output, block_index++);
    return processor.freeze_sampler_status().frozen &&
           max_abs_error_from(output, expected) < 0.001;
}

struct AdapterMemStream {
    std::vector<std::uint8_t> bytes;
    std::size_t cursor = 0;
};

int64_t adapter_mem_write(const clap_ostream_t* stream, const void* data, uint64_t size) {
    auto* mem = static_cast<AdapterMemStream*>(stream->ctx);
    auto* bytes = static_cast<const std::uint8_t*>(data);
    mem->bytes.insert(mem->bytes.end(), bytes, bytes + size);
    return static_cast<int64_t>(size);
}

int64_t adapter_mem_read(const clap_istream_t* stream, void* data, uint64_t size) {
    auto* mem = static_cast<AdapterMemStream*>(stream->ctx);
    const auto remaining = mem->bytes.size() - mem->cursor;
    const auto count = std::min<std::size_t>(remaining, static_cast<std::size_t>(size));
    if (count == 0) return 0;
    std::memcpy(data, mem->bytes.data() + mem->cursor, count);
    mem->cursor += count;
    return static_cast<int64_t>(count);
}

int save_clap_adapter_frozen_loop_state(const std::vector<std::uint8_t>& frozen_blob,
                                        std::vector<std::uint8_t>& saved_state) {
    configure_adapter_test_factory();
    const auto* factory = &pulp::format::clap_generic::plugin_factory;
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    CHECK(desc != nullptr, "CLAP adapter exposes V2 descriptor for state save");

    const clap_plugin_t* saver = factory->create_plugin(factory, nullptr, desc->id);
    CHECK(saver != nullptr, "CLAP adapter creates V2 saver instance for state save");
    CHECK(saver->init(saver), "CLAP adapter initializes V2 saver instance for state save");
    CHECK(saver->activate(saver, 48000.0, 64, 64),
          "CLAP adapter activates V2 saver instance for state save");
    auto* saver_processor = last_v2_adapter_processor();
    CHECK(saver_processor != nullptr, "CLAP adapter exposes V2 saver processor for state save");
    saver_processor->state().set_value(kFreeze, 1.0f);
    CHECK(saver_processor->deserialize_plugin_state(frozen_blob),
          "CLAP adapter saver accepts frozen loop payload for state save");

    auto* saver_state = static_cast<const clap_plugin_state_t*>(
        saver->get_extension(saver, CLAP_EXT_STATE));
    CHECK(saver_state != nullptr, "CLAP adapter exposes state extension for state save");
    AdapterMemStream saved_stream;
    clap_ostream_t ostream{};
    ostream.ctx = &saved_stream;
    ostream.write = adapter_mem_write;
    CHECK(saver_state->save(saver, &ostream), "CLAP adapter saves frozen loop state");
    CHECK(!saved_stream.bytes.empty(), "CLAP adapter writes non-empty frozen loop state");

    saver->deactivate(saver);
    saver->destroy(saver);
    pulp::format::clap_generic::g_factory = nullptr;
    saved_state = std::move(saved_stream.bytes);
    return 0;
}

int save_clap_adapter_released_loop_state(const std::vector<std::uint8_t>& frozen_blob,
                                          std::vector<std::uint8_t>& saved_state) {
    configure_adapter_test_factory();
    const auto* factory = &pulp::format::clap_generic::plugin_factory;
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    CHECK(desc != nullptr, "CLAP adapter exposes V2 descriptor for released state save");

    const clap_plugin_t* saver = factory->create_plugin(factory, nullptr, desc->id);
    CHECK(saver != nullptr, "CLAP adapter creates V2 saver instance for released state save");
    CHECK(saver->init(saver),
          "CLAP adapter initializes V2 saver instance for released state save");
    CHECK(saver->activate(saver, 48000.0, 64, 64),
          "CLAP adapter activates V2 saver instance for released state save");
    auto* saver_processor = last_v2_adapter_processor();
    CHECK(saver_processor != nullptr,
          "CLAP adapter exposes V2 saver processor for released state save");
    saver_processor->state().set_value(kFreeze, 1.0f);
    CHECK(saver_processor->deserialize_plugin_state(frozen_blob),
          "CLAP adapter saver accepts frozen loop payload before release");
    saver_processor->state().set_value(kFreeze, 0.0f);
    CHECK(saver_processor->serialize_plugin_state().empty(),
          "CLAP adapter saver omits frozen loop payload after release");

    auto* saver_state = static_cast<const clap_plugin_state_t*>(
        saver->get_extension(saver, CLAP_EXT_STATE));
    CHECK(saver_state != nullptr, "CLAP adapter exposes state extension for released state save");
    AdapterMemStream saved_stream;
    clap_ostream_t ostream{};
    ostream.ctx = &saved_stream;
    ostream.write = adapter_mem_write;
    CHECK(saver_state->save(saver, &ostream), "CLAP adapter saves released loop state");
    CHECK(!saved_stream.bytes.empty(), "CLAP adapter writes non-empty released loop state");
    const std::string state_text(reinterpret_cast<const char*>(saved_stream.bytes.data()),
                                 saved_stream.bytes.size());
    CHECK(state_text.find("PAV2FRZ1") == std::string::npos,
          "CLAP adapter released state omits the frozen loop payload magic");

    saver->deactivate(saver);
    saver->destroy(saver);
    pulp::format::clap_generic::g_factory = nullptr;
    saved_state = std::move(saved_stream.bytes);
    return 0;
}

int check_clap_adapter_frozen_loop_state_round_trip(
    const std::vector<std::uint8_t>& frozen_blob,
    float expected) {
    std::vector<std::uint8_t> saved_state;
    const int save_result = save_clap_adapter_frozen_loop_state(frozen_blob, saved_state);
    if (save_result != 0) return save_result;

    configure_adapter_test_factory();
    const auto* factory = &pulp::format::clap_generic::plugin_factory;
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    CHECK(desc != nullptr, "CLAP adapter exposes V2 descriptor");

    const clap_plugin_t* loader = factory->create_plugin(factory, nullptr, desc->id);
    CHECK(loader != nullptr, "CLAP adapter creates V2 loader instance");
    CHECK(loader->init(loader), "CLAP adapter initializes V2 loader instance");
    auto* loader_processor = last_v2_adapter_processor();
    CHECK(loader_processor != nullptr, "CLAP adapter exposes V2 loader processor");
    auto* loader_state = static_cast<const clap_plugin_state_t*>(
        loader->get_extension(loader, CLAP_EXT_STATE));
    CHECK(loader_state != nullptr, "CLAP adapter loader exposes state extension");

    AdapterMemStream input_stream;
    input_stream.bytes = saved_state;
    clap_istream_t istream{};
    istream.ctx = &input_stream;
    istream.read = adapter_mem_read;
    CHECK(loader_state->load(loader, &istream),
          "CLAP adapter restores frozen loop state into a fresh V2 instance");
    CHECK(loader_processor->state().get_value(kFreeze) >= 0.5f,
          "CLAP adapter restores the Freeze parameter");
    CHECK(loader->activate(loader, 48000.0, 64, 64),
          "CLAP adapter activates V2 loader instance after state restore");
    CHECK(adapter_recall_renders_frozen_loop(*loader_processor, expected),
          "CLAP adapter-restored V2 instance renders the frozen loop");

    loader->deactivate(loader);
    loader->destroy(loader);
    pulp::format::clap_generic::g_factory = nullptr;
    return 0;
}

int check_hosted_clap_frozen_loop_state_round_trip(
    const std::vector<std::uint8_t>& frozen_blob,
    float expected) {
#if defined(PROMPTABLE_ACCOMPANIST_V2_CLAP_BUNDLE_PATH)
    std::vector<std::uint8_t> saved_state;
    const int save_result = save_clap_adapter_frozen_loop_state(frozen_blob, saved_state);
    if (save_result != 0) return save_result;

    const std::filesystem::path clap_path = PROMPTABLE_ACCOMPANIST_V2_CLAP_BUNDLE_PATH;
    CHECK(std::filesystem::exists(clap_path),
          "hosted CLAP recall test has a built PromptableAccompanistV2 bundle");

    auto binary_path = clap_path;
    if (std::filesystem::is_directory(clap_path)) {
        binary_path = clap_path / "Contents" / "MacOS" / clap_path.stem();
    }
    void* handle = dlopen(binary_path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr, "hosted CLAP recall test dlopens the built V2 binary");
    auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    CHECK(entry != nullptr, "hosted CLAP recall test resolves clap_entry");
    CHECK(entry->init != nullptr && entry->get_factory != nullptr && entry->deinit != nullptr,
          "hosted CLAP recall test has a complete clap_entry");
    CHECK(entry->init(clap_path.string().c_str()), "hosted CLAP recall test initializes entry");

    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    CHECK(factory != nullptr, "hosted CLAP recall test gets the plugin factory");
    const clap_plugin_descriptor_t* desc = nullptr;
    const auto count = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* candidate = factory->get_plugin_descriptor(factory, i);
        if (candidate && candidate->id &&
            std::strcmp(candidate->id, "com.pulp.magenta.accompanist.v2") == 0) {
            desc = candidate;
            break;
        }
    }
    CHECK(desc != nullptr, "hosted CLAP recall test finds the V2 descriptor");

    clap_host_t host{};
    host.clap_version = CLAP_VERSION_INIT;
    host.name = "PromptableAccompanistV2 recall test";
    host.vendor = "PulpMagenta";
    host.url = "";
    host.version = "0.1.0";
    host.get_extension = [](const clap_host_t*, const char*) -> const void* {
        return nullptr;
    };
    host.request_restart = [](const clap_host_t*) {};
    host.request_process = [](const clap_host_t*) {};
    host.request_callback = [](const clap_host_t*) {};

    const clap_plugin_t* plugin = factory->create_plugin(factory, &host, desc->id);
    CHECK(plugin != nullptr, "hosted CLAP recall test creates the V2 plugin instance");
    CHECK(plugin->init(plugin), "hosted CLAP recall test initializes the V2 plugin instance");

    const auto* state = static_cast<const clap_plugin_state_t*>(
        plugin->get_extension(plugin, CLAP_EXT_STATE));
    CHECK(state != nullptr && state->load != nullptr && state->save != nullptr,
          "hosted CLAP recall test gets the state extension");
    AdapterMemStream input_stream;
    input_stream.bytes = saved_state;
    clap_istream_t istream{};
    istream.ctx = &input_stream;
    istream.read = adapter_mem_read;
    CHECK(state->load(plugin, &istream),
          "hosted V2 CLAP binary restores adapter-saved frozen loop state");

    const auto* params = static_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    CHECK(params != nullptr && params->get_value != nullptr,
          "hosted CLAP recall test gets the params extension");
    double freeze_value = 0.0;
    CHECK(params->get_value(plugin, kFreeze, &freeze_value),
          "hosted V2 CLAP binary exposes the Freeze parameter");
    CHECK(freeze_value >= 0.5,
          "hosted V2 CLAP binary exposes the restored Freeze value");

    CHECK(plugin->activate(plugin, 48000.0, 64, 64),
          "hosted V2 CLAP binary activates after recall");
    CHECK(plugin->start_processing(plugin),
          "hosted V2 CLAP binary starts processing after recall");

    pulp::audio::Buffer<float> output(2, 64);
    auto left = output.channel(0).data();
    auto right = output.channel(1).data();
    float* output_channels[2] = {left, right};
    clap_audio_buffer_t out_buffer{};
    out_buffer.data32 = output_channels;
    out_buffer.channel_count = 2;

    clap_input_events_t input_events{};
    input_events.size = [](const clap_input_events_t*) -> std::uint32_t {
        return 0;
    };
    input_events.get = [](const clap_input_events_t*, std::uint32_t)
        -> const clap_event_header_t* {
        return nullptr;
    };
    clap_output_events_t output_events{};
    output_events.try_push = [](const clap_output_events_t*,
                                const clap_event_header_t*) -> bool {
        return true;
    };
    clap_process_t process{};
    process.frames_count = 64;
    process.audio_outputs = &out_buffer;
    process.audio_outputs_count = 1;
    process.in_events = &input_events;
    process.out_events = &output_events;
    CHECK(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR,
          "hosted V2 CLAP binary processes after recall");
    CHECK(max_abs_error_from(output, expected) < 0.001,
          "hosted V2 CLAP binary renders the restored frozen loop");

    AdapterMemStream saved_again;
    clap_ostream_t ostream{};
    ostream.ctx = &saved_again;
    ostream.write = adapter_mem_write;
    CHECK(state->save(plugin, &ostream),
          "hosted V2 CLAP binary reserializes the restored state");
    CHECK(!saved_again.bytes.empty(),
          "hosted V2 CLAP binary writes non-empty recalled state");

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(handle);
    return 0;
#else
    (void)frozen_blob;
    (void)expected;
    return 0;
#endif
}

int dump_clap_adapter_frozen_loop_state_if_requested(
    const std::vector<std::uint8_t>& frozen_blob) {
    const char* output_path = std::getenv("PULP_MAGENTA_V2_DUMP_CLAP_STATE");
    if (output_path && *output_path) {
        std::vector<std::uint8_t> saved_state;
        const int save_result = save_clap_adapter_frozen_loop_state(frozen_blob, saved_state);
        if (save_result != 0) return save_result;

        std::ofstream output(output_path, std::ios::binary);
        CHECK(output.is_open(), "dump CLAP frozen loop state opens output file");
        output.write(reinterpret_cast<const char*>(saved_state.data()),
                     static_cast<std::streamsize>(saved_state.size()));
        CHECK(output.good(), "dump CLAP frozen loop state writes output file");
    }

    output_path = std::getenv("PULP_MAGENTA_V2_DUMP_CLAP_RELEASED_STATE");
    if (output_path && *output_path) {
        std::vector<std::uint8_t> saved_state;
        const int save_result = save_clap_adapter_released_loop_state(frozen_blob, saved_state);
        if (save_result != 0) return save_result;

        std::ofstream output(output_path, std::ios::binary);
        CHECK(output.is_open(), "dump CLAP released loop state opens output file");
        output.write(reinterpret_cast<const char*>(saved_state.data()),
                     static_cast<std::streamsize>(saved_state.size()));
        CHECK(output.good(), "dump CLAP released loop state writes output file");
    }

    return 0;
}

#if defined(__APPLE__) && defined(PROMPTABLE_ACCOMPANIST_V2_HAS_AUSDK)
int check_auv2_adapter_frozen_loop_state_round_trip(
    const std::vector<std::uint8_t>& frozen_blob,
    float expected) {
    configure_adapter_test_factory();

    pulp::format::au::PulpAUInstrument saver(nullptr);
    auto* saver_processor = last_v2_adapter_processor();
    CHECK(saver_processor != nullptr, "AUv2 adapter exposes V2 saver processor");
    prepare_v2_processor_for_adapter_recall(*saver_processor);
    saver_processor->state().set_value(kFreeze, 1.0f);
    CHECK(saver_processor->deserialize_plugin_state(frozen_blob),
          "AUv2 adapter saver accepts frozen loop payload");

    CFPropertyListRef saved = nullptr;
    CHECK(saver.SaveState(&saved) == noErr, "AUv2 adapter saves frozen loop state");
    CHECK(saved != nullptr, "AUv2 adapter writes non-empty frozen loop state");

    pulp::format::au::PulpAUInstrument loader(nullptr);
    auto* loader_processor = last_v2_adapter_processor();
    CHECK(loader_processor != nullptr, "AUv2 adapter exposes V2 loader processor");
    CHECK(loader.RestoreState(saved) == noErr,
          "AUv2 adapter restores frozen loop state into a fresh V2 instance");
    CHECK(loader_processor->state().get_value(kFreeze) >= 0.5f,
          "AUv2 adapter restores the Freeze parameter");
    prepare_v2_processor_for_adapter_recall(*loader_processor);
    CHECK(adapter_recall_renders_frozen_loop(*loader_processor, expected),
          "AUv2 adapter-restored V2 instance renders the frozen loop");

    CFRelease(saved);
    return 0;
}
#endif

int check_adapter_frozen_loop_state_round_trip() {
    constexpr float kExpectedSample = 0.25f;
    const auto frozen_blob = make_adapter_test_frozen_loop_blob(kExpectedSample);
    CHECK(!frozen_blob.empty(), "adapter frozen loop payload serializes");

    const auto home = unique_temp_dir("pulp-magenta-v2-adapter-state");
    const auto pulp_home = home / ".pulp";
    const auto home_string = home.string();
    const auto pulp_home_string = pulp_home.string();
    EnvGuard home_guard("HOME", home_string.c_str());
    EnvGuard pulp_home_guard("PULP_HOME", pulp_home_string.c_str());
    EnvGuard explicit_model_guard("MRT2_MODEL", nullptr);
    EnvGuard worker_guard("PULP_MAGENTA_V2_TEST_DISABLE_WORKER", "1");

    const int clap_result =
        check_clap_adapter_frozen_loop_state_round_trip(frozen_blob, kExpectedSample);
    if (clap_result != 0) return clap_result;

    const int hosted_clap_result =
        check_hosted_clap_frozen_loop_state_round_trip(frozen_blob, kExpectedSample);
    if (hosted_clap_result != 0) return hosted_clap_result;

    const int dump_clap_result =
        dump_clap_adapter_frozen_loop_state_if_requested(frozen_blob);
    if (dump_clap_result != 0) return dump_clap_result;

#if defined(__APPLE__) && defined(PROMPTABLE_ACCOMPANIST_V2_HAS_AUSDK)
    const int auv2_result =
        check_auv2_adapter_frozen_loop_state_round_trip(frozen_blob, kExpectedSample);
    if (auv2_result != 0) return auv2_result;
#endif

    std::filesystem::remove_all(home);
    return 0;
}

std::string alternate_model_path_for_runtime_smoke(const std::string& current) {
    const auto shared = pulp::runtime::resolve_pulp_home() / "magenta/models";
    const auto legacy = magenta_demo::legacy_magenta_models_root();
    const std::array<std::filesystem::path, 4> candidates = {
        shared / "mrt2_base/mrt2_base.mlxfn",
        shared / "mrt2_small/mrt2_small.mlxfn",
        legacy / "mrt2_base/mrt2_base.mlxfn",
        legacy / "mrt2_small/mrt2_small.mlxfn",
    };
    for (const auto& candidate : candidates)
        if (candidate.string() != current && model_bundle_usable(candidate))
            return candidate.string();
    return {};
}

bool wait_for_loaded_model_path(Processor& processor,
                                pulp::audio::Buffer<float>& output,
                                std::uint64_t& block_index,
                                const std::string& expected_path,
                                int max_blocks) {
    for (int i = 0; i < max_blocks; ++i) {
        process_runtime_block(processor, output, block_index++);
        sleep_for_runtime_block(output);
        if (processor.loaded_model_path_for_test() == expected_path) return true;
        const auto status = processor.runtime_status_text();
        if (status.find("failed") != std::string::npos ||
            status.find("stopped") != std::string::npos)
            return false;
    }
    return false;
}

bool wait_for_runtime_status_containing(Processor& processor,
                                        pulp::audio::Buffer<float>& output,
                                        std::uint64_t& block_index,
                                        const std::string& needle,
                                        int max_blocks) {
    for (int i = 0; i < max_blocks; ++i) {
        process_runtime_block(processor, output, block_index++);
        sleep_for_runtime_block(output);
        if (processor.runtime_status_text().find(needle) != std::string::npos)
            return true;
    }
    return false;
}

bool wait_for_prompt_change_applied(Processor& processor,
                                    pulp::audio::Buffer<float>& output,
                                    std::uint64_t& block_index,
                                    int max_blocks) {
    for (int i = 0; i < max_blocks; ++i) {
        process_runtime_block(processor, output, block_index++);
        sleep_for_runtime_block(output);
        if (processor.prompt_change_applied_for_test()) return true;
    }
    return false;
}

int run_generated_runtime_smoke_if_requested() {
    const char* enabled = std::getenv("PULP_MAGENTA_V2_RUN_MODEL_SMOKE");
    if (enabled == nullptr || std::string(enabled) != "1") return 0;

    std::error_code ec;
    CHECK(std::filesystem::exists(default_resources(), ec),
          "runtime smoke requires installed MRT2 shared resources");
    ec.clear();
    CHECK(std::filesystem::exists(default_model(), ec),
          "runtime smoke requires an installed MRT2 model");

    Processor processor;
    pulp::state::StateStore state;
    processor.set_state_store(&state);
    processor.define_parameters(state);
    state.set_value(kVolumeDb, 0.0f);
    state.set_value(kCaptureSeconds, 0.25f);
    state.set_value(kLoopCrossfadeMs, 10.0f);
    state.set_value(kFreeze, 0.0f);

    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 512;
    prepare.input_channels = 0;
    prepare.output_channels = 2;
    processor.prepare(prepare);

    pulp::audio::Buffer<float> output(2, 512);
    std::uint64_t block_index = 0;
    constexpr double kAudibleRms = 1.0e-6;
    CHECK(wait_for_generated_audio(processor, output, block_index, kAudibleRms, 2400),
          "runtime smoke generated non-silent model audio");

    for (int i = 0; i < 32; ++i) {
        process_runtime_block(processor, output, block_index++);
        sleep_for_runtime_block(output);
    }

    state.set_value(kFreeze, 1.0f);
    CHECK(wait_for_frozen_runtime_sample(processor, output, block_index, 1, kAudibleRms, 250),
          "runtime smoke freezes generated audio into a published loop");
    const auto first_freeze = processor.freeze_sampler_status();
    CHECK(first_freeze.sample_frames > 0, "runtime smoke frozen sample has frames");
    CHECK(first_freeze.materialize_failures == 0,
          "runtime smoke first freeze has no materialization failure");

    double held_loop_rms = 0.0;
    for (int i = 0; i < 16; ++i) {
        process_runtime_block(processor, output, block_index++);
        held_loop_rms = std::max(held_loop_rms, rms_audio(output));
        sleep_for_runtime_block(output);
    }
    CHECK(processor.freeze_sampler_status().frozen,
          "runtime smoke keeps frozen loop active while Freeze is held");
    CHECK(held_loop_rms >= kAudibleRms,
          "runtime smoke frozen loop playback remains audible");

    state.set_value(kFreeze, 0.0f);
    CHECK(wait_for_released_runtime_sample(processor, output, block_index, 120),
          "runtime smoke releases frozen loop back to live generation");
    CHECK(!processor.freeze_sampler_status().frozen,
          "runtime smoke exits frozen state after release");

    CHECK(wait_for_generated_audio(processor, output, block_index, kAudibleRms, 240),
          "runtime smoke resumes generated audio after release");

    processor.set_prompt_for_test("   \n");
    CHECK(!processor.prompt_change_applied_for_test(),
          "runtime smoke clear prompt waits for the prompt debounce");
    CHECK(wait_for_prompt_change_applied(processor, output, block_index, 240),
          "runtime smoke eventually applies a settled clear prompt");
    CHECK(wait_for_generated_audio(processor, output, block_index, kAudibleRms, 240),
          "runtime smoke keeps generated audio alive when the prompt is cleared");
    CHECK(processor.runtime_status_text().find("failed") == std::string::npos,
          "runtime smoke clear prompt does not fail model encoders");

    auto check_bad_reload_preserves_current = [&]() -> int {
        const auto loaded_before_bad_reload = processor.loaded_model_path_for_test();
        const auto bad_reload = unique_temp_dir("pulp-magenta-v2-bad-reload") /
                                "mrt2_small/mrt2_small.mlxfn";
        processor.request_model_reload_for_test(bad_reload.string());
        CHECK(wait_for_runtime_status_containing(processor,
                                                 output,
                                                 block_index,
                                                 "download or repair a model",
                                                 240),
              "runtime smoke reports an incomplete requested model");
        CHECK(processor.loaded_model_path_for_test() == loaded_before_bad_reload,
              "runtime smoke keeps the previous model selected after rejected reload");
        CHECK(wait_for_generated_audio(processor, output, block_index, kAudibleRms, 240),
              "runtime smoke keeps generated audio alive after rejected reload");
        return 0;
    };

    const std::string hot_switch_model = alternate_model_path_for_runtime_smoke(default_model());
    if (!hot_switch_model.empty()) {
        processor.request_model_reload_for_test(hot_switch_model);
        CHECK(wait_for_loaded_model_path(processor, output, block_index, hot_switch_model, 2400),
              "runtime smoke hot-switches to another installed model");
        const auto status = processor.runtime_status_text();
        CHECK(status.find("encoders failed") == std::string::npos,
              "runtime smoke hot model switch keeps MusicCoCa encoders available");
        CHECK(wait_for_generated_audio(processor, output, block_index, kAudibleRms, 2400),
              "runtime smoke generated audio after hot model switch");
    }
    if (const int bad_reload_result = check_bad_reload_preserves_current(); bad_reload_result != 0)
        return bad_reload_result;

    for (int i = 0; i < 32; ++i) {
        process_runtime_block(processor, output, block_index++);
        sleep_for_runtime_block(output);
    }

    state.set_value(kFreeze, 1.0f);
    CHECK(wait_for_frozen_runtime_sample(processor, output, block_index, 2, kAudibleRms, 250),
          "runtime smoke recaptures generated audio on the next hold");
    const auto second_freeze = processor.freeze_sampler_status();
    CHECK(second_freeze.captures_completed >= first_freeze.captures_completed + 1,
          "runtime smoke publishes a replacement frozen sample");
    CHECK(second_freeze.materialize_failures == 0,
          "runtime smoke recapture has no materialization failure");

    state.set_value(kFreeze, 0.0f);
    processor.release();
    return 0;
}

}  // namespace

int main() {
    const int registry_result = check_model_registry_defaults();
    if (registry_result != 0) return registry_result;
    const int active_bundle_result = check_active_model_requires_complete_bundle();
    if (active_bundle_result != 0) return active_bundle_result;
    const int m1_resolver_result = check_m1_resolver_avoids_large_model();
    if (m1_resolver_result != 0) return m1_resolver_result;
    const int runtime_status_result = check_runtime_status_priority();
    if (runtime_status_result != 0) return runtime_status_result;
    const int worker_preflight_result = check_worker_preflight_rejections_do_not_publish_loading();
    if (worker_preflight_result != 0) return worker_preflight_result;
    const int status_banner_result = check_status_banner_refreshes_after_late_frame_clock();
    if (status_banner_result != 0) return status_banner_result;
    const int model_section_result = check_model_section_surfaces_storage_locations();
    if (model_section_result != 0) return model_section_result;
    const int prompt_clear_result = check_prompt_clear_contract();
    if (prompt_clear_result != 0) return prompt_clear_result;
    const int generation_watchdog_result = check_generation_watchdog_detects_stagnant_frames();
    if (generation_watchdog_result != 0) return generation_watchdog_result;
    const int package_audit_audio_result = check_package_audit_mutes_generated_audio_by_default();
    if (package_audit_audio_result != 0) return package_audit_audio_result;
    const int frozen_loop_state_result = check_frozen_loop_state_round_trip();
    if (frozen_loop_state_result != 0) return frozen_loop_state_result;
    const int adapter_state_result = check_adapter_frozen_loop_state_round_trip();
    if (adapter_state_result != 0) return adapter_state_result;

    Processor p;
    auto d = p.descriptor();
    CHECK(d.name == "PromptableAccompanistV2", "descriptor name is v2");
    CHECK(d.category == pulp::format::PluginCategory::Instrument, "category is Instrument");
    CHECK(d.accepts_midi, "accepts MIDI");
    CHECK(d.input_buses.empty(), "no audio input bus");
    CHECK(d.output_buses.size() == 1, "one output bus");
    CHECK(d.output_buses[0].default_channels == 2, "stereo output");

    pulp::state::StateStore store;
    p.define_parameters(store);
    CHECK(store.all_params().size() == kParameterCount, "nine automatable params");

    FreezeLoopSampler sampler;
    FreezeLoopSamplerConfig config;
    config.num_channels = 2;
    config.sample_rate = 48000.0;
    config.max_block_frames = 64;
    config.max_capture_seconds = 0.25;
    config.sample_slots = 2;
    CHECK(sampler.prepare(config), "freeze sampler prepares");

    pulp::audio::Buffer<float> block(2, 64);
    FreezeLoopSamplerControls live_controls;
    live_controls.freeze = false;
    live_controls.capture_seconds = 0.05;
    live_controls.loop_crossfade_ms = 5.0;
    for (int i = 0; i < 16; ++i) {
        fill_test_audio(block, i);
        auto view = block.view();
        sampler.process(view, live_controls);
    }

    FreezeLoopSamplerControls freeze_controls = live_controls;
    freeze_controls.freeze = true;
    fill_test_audio(block, 16);
    auto freeze_view = block.view();
    sampler.process(freeze_view, freeze_controls);
    CHECK(wait_for_frozen_sample(sampler, block, freeze_controls), "freeze sampler publishes held loop");

    const auto frozen_status = sampler.status();
    CHECK(frozen_status.sample_frames > 0, "published sample has frames");
    CHECK(frozen_status.materialize_failures == 0, "materialization succeeds");

    fill_test_audio(block, 400);
    auto release_view = block.view();
    sampler.process(release_view, live_controls);
    sampler.shutdown();

    FreezeLoopSampler quick_sampler;
    CHECK(quick_sampler.prepare(config), "quick release sampler prepares");
    for (int i = 0; i < 16; ++i) {
        fill_constant_audio(block, 0.125f);
        auto view = block.view();
        quick_sampler.process(view, live_controls);
    }
    fill_constant_audio(block, 0.125f);
    auto quick_freeze_view = block.view();
    quick_sampler.process(quick_freeze_view, freeze_controls);
    fill_constant_audio(block, 0.0f);
    auto quick_release_view = block.view();
    quick_sampler.process(quick_release_view, live_controls);
    fill_constant_audio(block, -0.125f);
    auto quick_refreeze_view = block.view();
    quick_sampler.process(quick_refreeze_view, freeze_controls);
    CHECK(wait_for_frozen_sample(quick_sampler, block, freeze_controls),
          "quick release/re-press publishes a loop");
    CHECK(quick_sampler.status().materialize_failures == 0,
          "quick release/re-press has no materialization failure");
    quick_sampler.shutdown();

    FreezeLoopSampler contract_sampler;
    CHECK(contract_sampler.prepare(config), "audible contract sampler prepares");
    for (int i = 0; i < 17; ++i) {
        fill_constant_audio(block, 0.125f);
        auto view = block.view();
        contract_sampler.process(view, live_controls);
    }
    fill_constant_audio(block, 0.125f);
    auto contract_freeze_view = block.view();
    contract_sampler.process(contract_freeze_view, freeze_controls);
    CHECK(wait_for_frozen_sample(contract_sampler, block, freeze_controls),
          "audible contract sampler publishes first held loop");
    const auto first_capture_count = contract_sampler.status().captures_completed;
    CHECK(first_capture_count == 1, "first held loop counts as one capture");
    const auto frozen_error = process_constant_blocks_and_measure_error(contract_sampler,
                                                                        block,
                                                                        freeze_controls,
                                                                        0.75f,
                                                                        0.125,
                                                                        24,
                                                                        4);
    CHECK(frozen_error < 0.05,
          "frozen loop renders held sample while live input changes");

    const auto released_error = process_constant_blocks_and_measure_error(contract_sampler,
                                                                          block,
                                                                          live_controls,
                                                                          -0.5f,
                                                                          -0.5,
                                                                          24,
                                                                          4);
    CHECK(released_error < 0.05,
          "release returns output to live input");
    CHECK(!contract_sampler.status().frozen, "release exits frozen playback state");

    for (int i = 0; i < 40; ++i) {
        fill_constant_audio(block, -0.25f);
        auto view = block.view();
        contract_sampler.process(view, live_controls);
    }
    fill_constant_audio(block, -0.25f);
    auto contract_refreeze_view = block.view();
    contract_sampler.process(contract_refreeze_view, freeze_controls);
    CHECK(wait_for_frozen_sample(contract_sampler, block, freeze_controls),
          "audible contract sampler publishes replacement held loop");
    CHECK(contract_sampler.status().captures_completed == first_capture_count + 1,
          "recapture replaces the frozen sample on the next hold");
    const auto recaptured_error = process_constant_blocks_and_measure_error(contract_sampler,
                                                                            block,
                                                                            freeze_controls,
                                                                            0.75f,
                                                                            -0.25,
                                                                            24,
                                                                            4);
    CHECK(recaptured_error < 0.05,
          "recaptured loop renders the newer held sample");
    CHECK(contract_sampler.status().materialize_failures == 0,
          "audible contract path has no materialization failure");
    contract_sampler.shutdown();

    std::vector<float> keyed_root;
    std::vector<float> keyed_octave;
    CHECK(render_keyed_frozen_note(60, keyed_root),
          "keyed freeze sampler renders root note");
    CHECK(render_keyed_frozen_note(72, keyed_octave),
          "keyed freeze sampler renders octave note");
    double keyed_root_energy = 0.0;
    double keyed_octave_energy = 0.0;
    double keyed_delta = 0.0;
    const auto keyed_count = std::min(keyed_root.size(), keyed_octave.size());
    for (std::size_t i = 0; i < keyed_count; ++i) {
        keyed_root_energy += static_cast<double>(keyed_root[i]) * keyed_root[i];
        keyed_octave_energy += static_cast<double>(keyed_octave[i]) * keyed_octave[i];
        keyed_delta = std::max(keyed_delta,
                               std::fabs(static_cast<double>(keyed_root[i] - keyed_octave[i])));
    }
    CHECK(keyed_count > 0, "keyed freeze sampler captures comparison output");
    CHECK(std::sqrt(keyed_root_energy / static_cast<double>(keyed_count)) > 1.0e-4,
          "keyed freeze sampler root note is audible");
    CHECK(std::sqrt(keyed_octave_energy / static_cast<double>(keyed_count)) > 1.0e-4,
          "keyed freeze sampler octave note is audible");
    CHECK(keyed_delta > 1.0e-3,
          "keyed freeze sampler changes playback rate by MIDI note");

    FreezeLoopSampler shape_sampler;
    CHECK(shape_sampler.prepare(config), "shape guard sampler prepares");
    pulp::audio::Buffer<float> oversized_block(2, 65);
    auto oversized_view = oversized_block.view();
    shape_sampler.process(oversized_view, live_controls);
    CHECK(shape_sampler.status().buffer_shape_mismatches == 1,
          "oversized block is rejected before capture");
    shape_sampler.shutdown();

    const int runtime_smoke_result = run_generated_runtime_smoke_if_requested();
    if (runtime_smoke_result != 0) return runtime_smoke_result;

    std::puts("OK: Promptable Accompanist V2 instrument contract + freeze loop sampler");
    return 0;
}
