// PromptableAccompanistV2 VST3 entry point (instrument).
#include "accompanist.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PromptableAccompanistV2UID(
    0x50554C50, 0x4D414743, 0x41433200, 0x00000002);   // 'PULP' 'MAGC' 'AC2\0' v2

PULP_VST3_PLUGIN(PromptableAccompanistV2UID, "PromptableAccompanistV2",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "PulpMagenta", "0.1.0",
                 "https://github.com/danielraffel/pulp-magenta-examples",
                 pulp::examples::accompanist_v2::create_promptable_accompanist_v2)
