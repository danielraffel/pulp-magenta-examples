// PromptableAccompanist VST3 entry point (instrument).
#include "accompanist.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PromptableAccompanistUID(
    0x50554C50, 0x4D414743, 0x41434300, 0x00000001);   // 'PULP' 'MAGC' 'ACC\0' v1

PULP_VST3_PLUGIN(PromptableAccompanistUID, "PromptableAccompanist",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "PulpMagenta", "0.1.0",
                 "https://github.com/danielraffel/pulp-magenta-examples",
                 pulp::examples::accompanist::create_promptable_accompanist)
