// PromptableAccompanist — standalone host. Opens the plugin editor (the WebView UI)
// and runs the generative instrument. Pass --screenshot=PATH for a headless one-shot
// capture of the editor (UI validation / CI); otherwise it runs a normal window.
#include "accompanist.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <string>

int main(int argc, char** argv) {
    pulp::runtime::log_info("PromptableAccompanist Standalone v0.1.0");
    pulp::format::StandaloneApp app(
        pulp::examples::accompanist::create_promptable_accompanist);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;   // MRT2 is 48 kHz native
    config.buffer_size = 512;
    config.input_channels = 0;      // instrument: no audio input
    config.output_channels = 2;
    config.show_settings_tab = true;   // host Settings: [Audio][MIDI] + the plugin's Models tab
                                       // avoids a second host-provided Settings tab.

    // --screenshot=PATH → headless capture of the first painted editor frame.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--screenshot=", 0) == 0) {
            config.screenshot_path = a.substr(std::string("--screenshot=").size());
            config.screenshot_frame_delay = 120;  // give the WebView time to load + paint
            config.headless = true;
        }
    }
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("Failed to run standalone editor");
        return 1;
    }
    return 0;
}
