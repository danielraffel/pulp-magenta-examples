// Headless smoke test for the Promptable Accompanist processor. Validates the
// instrument contract (descriptor + parameters) WITHOUT loading the model — so it
// runs fast in CI and proves the magentart::core link + Processor wiring compile.
#include <catch2/catch_test_macros.hpp>
#include "accompanist.hpp"

using namespace pulp::examples::accompanist;

TEST_CASE("Promptable Accompanist declares an instrument", "[accompanist][descriptor]") {
    Processor p;
    auto d = p.descriptor();
    REQUIRE(d.category == pulp::format::PluginCategory::Instrument);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.input_buses.empty());
    REQUIRE(d.output_buses.size() == 1);
    REQUIRE(d.output_buses[0].default_channels == 2);
}

TEST_CASE("Promptable Accompanist registers automatable numeric params", "[accompanist][params]") {
    Processor p;
    pulp::state::StateStore store;
    p.define_parameters(store);
    REQUIRE(store.all_params().size() == 6);
}
