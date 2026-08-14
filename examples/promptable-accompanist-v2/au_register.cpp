// PromptableAccompanistV2 plugin registry (forces the factory to link into the AU host).
#include "accompanist.hpp"
#include <pulp/format/registry.hpp>
PULP_REGISTER_PLUGIN(pulp::examples::accompanist_v2::create_promptable_accompanist_v2)
extern "C" void promptable_accompanist_v2_force_link() {}
