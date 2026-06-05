// PromptableAccompanist AU v2 entry — INSTRUMENT (aumu / MusicDeviceBase).
// Uses PULP_AU_INSTRUMENT (AUMusicDeviceFactory) so the AU implements
// MusicDeviceMIDIEvent — required for an aumu to pass auval + take MIDI in Logic.
#include "accompanist.hpp"
#include <pulp/format/au_v2_instrument_entry.hpp>
PULP_AU_INSTRUMENT(PromptableAccompanistAU,
                   pulp::examples::accompanist::create_promptable_accompanist)
