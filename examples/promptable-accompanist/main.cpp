// PromptableAccompanist standalone host (instrument: MIDI in, audio out).
#include "accompanist.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

static std::atomic<bool> should_quit{false};
static void on_signal(int) { should_quit.store(true); }

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    pulp::runtime::log_info("PromptableAccompanist Standalone v0.1.0");

    pulp::format::StandaloneApp app(
        pulp::examples::accompanist::create_promptable_accompanist);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;   // MRT2 is 48 kHz native
    config.buffer_size = 512;
    config.input_channels = 0;      // instrument: no audio input
    config.output_channels = 2;
    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("Failed to start standalone app");
        return 1;
    }
    std::cout << "\nPromptableAccompanist is running (generative MRT2 instrument).\n"
              << "Play MIDI to steer; set MRT2_PROMPT to change the style.\n"
              << "Heads-up: this generates audio. Press Ctrl+C to quit.\n" << std::endl;
    while (!should_quit.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    app.stop();
    return 0;
}
