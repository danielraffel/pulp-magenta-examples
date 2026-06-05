// PromptableAccompanist plugin registry (forces the factory to link into the AU host).
#include "accompanist.hpp"
#include <pulp/format/registry.hpp>
PULP_REGISTER_PLUGIN(pulp::examples::accompanist::create_promptable_accompanist)
extern "C" void promptable_accompanist_force_link() {}
