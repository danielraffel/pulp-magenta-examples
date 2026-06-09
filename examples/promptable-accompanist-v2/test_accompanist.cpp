// Headless smoke test for the Promptable Accompanist V2 processor. Validates the
// instrument contract without loading the model, then exercises the freeze/loop helper
// against synthetic audio so the sampler primitives compile and publish a held sample.
#include "accompanist.hpp"
#include "freeze_loop_sampler.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <thread>

using namespace pulp::examples::accompanist_v2;

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); return 1; } } while (0)

namespace {

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

}  // namespace

int main() {
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

    FreezeLoopSampler shape_sampler;
    CHECK(shape_sampler.prepare(config), "shape guard sampler prepares");
    pulp::audio::Buffer<float> oversized_block(2, 65);
    auto oversized_view = oversized_block.view();
    shape_sampler.process(oversized_view, live_controls);
    CHECK(shape_sampler.status().buffer_shape_mismatches == 1,
          "oversized block is rejected before capture");
    shape_sampler.shutdown();

    std::puts("OK: Promptable Accompanist V2 instrument contract + freeze loop sampler");
    return 0;
}
