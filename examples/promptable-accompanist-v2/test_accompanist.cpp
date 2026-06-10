// Headless smoke test for the Promptable Accompanist V2 processor. Validates the
// instrument contract without loading the model, then exercises the freeze/loop helper
// against synthetic audio so the sampler primitives compile and publish a held sample.
#include "accompanist.hpp"
#include "freeze_loop_sampler.hpp"

#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace pulp::examples::accompanist_v2;

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); return 1; } } while (0)

namespace {

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
    std::filesystem::remove_all(home);
    return 0;
}

int check_active_model_requires_complete_bundle() {
    const auto home = unique_temp_dir("pulp-magenta-v2-active-bundle");
    const auto legacy_home = unique_temp_dir("pulp-magenta-v2-legacy-bundle");
    EnvGuard home_guard("HOME", legacy_home.c_str());
    EnvGuard pulp_home_guard("PULP_HOME", home.c_str());
    EnvGuard explicit_model_guard("MRT2_MODEL", nullptr);

    const auto shared = home / "magenta/models/mrt2_small/mrt2_small.mlxfn";
    touch_model_file(shared);
    write_text_file(home / "magenta/models/mrt2_small.json",
                    "{\n"
                    "  \"model_id\": \"mrt2_small\",\n"
                    "  \"backend\": \"mlx\",\n"
                    "  \"checkpoint_ref\": \"hf://google/magenta-realtime-2/models/mrt2_small/mrt2_small.mlxfn\",\n"
                    "  \"resolved_checkpoint_path\": \"" + shared.string() + "\"\n"
                    "}\n");
    write_text_file(home / "magenta/model-state.json",
                    "{\n"
                    "  \"active_model_id\": \"mrt2_small\",\n"
                    "  \"configured_model_id\": \"mrt2_small\",\n"
                    "  \"resolved_checkpoint_path\": \"" + shared.string() + "\"\n"
                    "}\n");

    const auto legacy_root = legacy_home / "Documents/Magenta/magenta-rt-v2/models";
    const auto legacy_small = legacy_root / "mrt2_small/mrt2_small.mlxfn";
    touch_model_file(legacy_small);
    touch_model_file(legacy_root / "mrt2_small/mrt2_small_state.safetensors");
    CHECK(default_model().empty(),
          "incomplete active shared model does not silently fall back to a legacy model");

    touch_model_file(home / "magenta/models/mrt2_small/mrt2_small_state.safetensors");
    CHECK(default_model() == shared.string(),
          "complete active shared model resolves to the shared checkpoint");

    std::filesystem::remove_all(home);
    std::filesystem::remove_all(legacy_home);
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
        [&model_ready] { return model_ready; },
        [&runtime_status] { return runtime_status; },
        [] {},
        "warm analog pads");
    auto* root_ptr = root.get();
    CHECK(root_ptr->child_count() == 3,
          "runtime status banner is rendered while the model is loading");

    pulp::view::View parent;
    parent.add_child(std::move(root));

    runtime_status.clear();
    pulp::view::FrameClock clock;
    parent.set_frame_clock(&clock);
    root_ptr->layout_children();
    CHECK(root_ptr->child_count() == 2,
          "late frame-clock attachment clears a stale loading banner");

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

std::string alternate_model_path_for_runtime_smoke(const std::string& current) {
    const auto shared = pulp::runtime::resolve_pulp_home() / "magenta/models";
    const auto legacy = std::filesystem::path(env_or("HOME", "")) /
                        "Documents/Magenta/magenta-rt-v2/models";
    const std::array<std::filesystem::path, 4> candidates = {
        shared / "mrt2_base/mrt2_base.mlxfn",
        shared / "mrt2_small/mrt2_small.mlxfn",
        legacy / "mrt2_base/mrt2_base.mlxfn",
        legacy / "mrt2_small/mrt2_small.mlxfn",
    };
    for (const auto& candidate : candidates)
        if (candidate.string() != current && model_bundle_complete(candidate))
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

        const auto loaded_before_bad_reload = processor.loaded_model_path_for_test();
        const auto bad_reload = unique_temp_dir("pulp-magenta-v2-bad-reload") /
                                "mrt2_small/mrt2_small.mlxfn";
        processor.request_model_reload_for_test(bad_reload.string());
        CHECK(wait_for_runtime_status_containing(processor,
                                                 output,
                                                 block_index,
                                                 "missing or incomplete",
                                                 240),
              "runtime smoke reports an incomplete requested model");
        CHECK(processor.loaded_model_path_for_test() == loaded_before_bad_reload,
              "runtime smoke keeps the previous model selected after rejected reload");
        CHECK(wait_for_generated_audio(processor, output, block_index, kAudibleRms, 240),
              "runtime smoke keeps generated audio alive after rejected reload");
    }

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
    const int status_banner_result = check_status_banner_refreshes_after_late_frame_clock();
    if (status_banner_result != 0) return status_banner_result;

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
