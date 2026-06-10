#pragma once

// Magenta RealTime 2 model registry (ModelManager PR4 — first use of the Pulp
// model-manager primitive). These ModelEntry rows describe the two MRT2 checkpoints
// hosted on HuggingFace (google/magenta-realtime-2, CC-BY-4.0 Google DeepMind). The
// weights are NOT bundled in the plugin — the ModelManagerView downloads them on demand
// via pulp::runtime::install_model("magenta", entry, …) into the shared model store,
// replacing the install-weights.sh shell flow. Both the large (mrt2_base) and small
// (mrt2_small) models are offered; the user sets a default and can remove either.
//
// In-repo paths mirror the local layout the `mrt` CLI uses:
//   models/<id>/<id>.mlxfn  +  models/<id>/<id>_state.safetensors
// (the shared resources/ — musiccoca, spectrostream — are initialised separately).

#include <pulp/runtime/model_registry.hpp>

#include <vector>

namespace magenta_demo {

inline constexpr char kMagentaSubsystem[] = "magenta";

inline const std::vector<pulp::runtime::ModelEntry>& magenta_models() {
    using pulp::runtime::ModelAsset;
    using pulp::runtime::ModelEntry;

    static const std::vector<ModelEntry> models = {
        ModelEntry{
            .model_id = "mrt2_small",
            .display_name = "Magenta RealTime 2 — Small",
            .description = "230M parameters. Recommended for first run and Apple Silicon Macs.",
            .backend = "mlx",
            .checkpoint_ref = "hf://google/magenta-realtime-2/models/mrt2_small/mrt2_small.mlxfn",
            .size_bytes = 443ULL * 1024 * 1024,
            .auto_downloadable = true,
            .assets =
                {
                    ModelAsset{.role = "weights",
                               .checkpoint_ref =
                                   "hf://google/magenta-realtime-2/models/mrt2_small/mrt2_small.mlxfn"},
                    ModelAsset{.role = "state",
                               .checkpoint_ref = "hf://google/magenta-realtime-2/models/mrt2_small/"
                                                 "mrt2_small_state.safetensors"},
                },
            .is_recommended = true,
            .license = "CC-BY-4.0",
            .attribution = "Google DeepMind",
            .min_device = "Apple silicon",
        },
        ModelEntry{
            .model_id = "mrt2_base",
            .display_name = "Magenta RealTime 2 — Large",
            .description = "2.4B parameters. Best quality; needs a high-end Pro/Max Mac.",
            .backend = "mlx",
            .checkpoint_ref = "hf://google/magenta-realtime-2/models/mrt2_base/mrt2_base.mlxfn",
            .size_bytes = 2600ULL * 1024 * 1024,
            .auto_downloadable = true,
            .assets =
                {
                    ModelAsset{.role = "weights",
                               .checkpoint_ref =
                                   "hf://google/magenta-realtime-2/models/mrt2_base/mrt2_base.mlxfn"},
                    ModelAsset{.role = "state",
                               .checkpoint_ref = "hf://google/magenta-realtime-2/models/mrt2_base/"
                                                 "mrt2_base_state.safetensors"},
                },
            .is_recommended = false,
            .license = "CC-BY-4.0",
            .attribution = "Google DeepMind",
            .min_device = "High-end Pro/Max",
        },
    };
    return models;
}

}  // namespace magenta_demo
