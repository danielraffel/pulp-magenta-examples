// Headless smoke test for the Promptable Accompanist processor. Validates the instrument
// contract (descriptor + parameters) WITHOUT loading the model — so it runs fast and proves
// the magentart::core link + Processor wiring compile. Plain main() (no Catch2): a downstream
// SDK-consuming project doesn't get Catch2 from the Pulp SDK.
#include "accompanist.hpp"
#include <cstdio>

using namespace pulp::examples::accompanist;

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); return 1; } } while (0)

int main() {
    Processor p;
    auto d = p.descriptor();
    CHECK(d.category == pulp::format::PluginCategory::Instrument, "category is Instrument");
    CHECK(d.accepts_midi, "accepts MIDI");
    CHECK(d.input_buses.empty(), "no audio input bus");
    CHECK(d.output_buses.size() == 1, "one output bus");
    CHECK(d.output_buses[0].default_channels == 2, "stereo output");

    pulp::state::StateStore store;
    p.define_parameters(store);
    CHECK(store.all_params().size() == 6, "six automatable params");

    std::puts("OK: Promptable Accompanist instrument contract + 6 params");
    return 0;
}
