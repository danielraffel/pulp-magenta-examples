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

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
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
            .size_bytes = 464331548ULL,
            .auto_downloadable = true,
            .assets =
                {
                    ModelAsset{.role = "weights",
                               .checkpoint_ref =
                                   "hf://google/magenta-realtime-2/models/mrt2_small/mrt2_small.mlxfn",
                               .size_bytes = 455654550ULL},
                    ModelAsset{.role = "state",
                               .checkpoint_ref = "hf://google/magenta-realtime-2/models/mrt2_small/"
                                                 "mrt2_small_state.safetensors",
                               .size_bytes = 8676998ULL},
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
            .size_bytes = 2788354715ULL,
            .auto_downloadable = true,
            .assets =
                {
                    ModelAsset{.role = "weights",
                               .checkpoint_ref =
                                   "hf://google/magenta-realtime-2/models/mrt2_base/mrt2_base.mlxfn",
                               .size_bytes = 2771414746ULL},
                    ModelAsset{.role = "state",
                               .checkpoint_ref = "hf://google/magenta-realtime-2/models/mrt2_base/"
                                                 "mrt2_base_state.safetensors",
                               .size_bytes = 16939969ULL},
                },
            .is_recommended = false,
            .license = "CC-BY-4.0",
            .attribution = "Google DeepMind",
            .min_device = "High-end Pro/Max",
        },
    };
    return models;
}

inline std::filesystem::path magenta_state_path_for_model(const std::filesystem::path& checkpoint) {
    return checkpoint.parent_path() / (checkpoint.stem().string() + "_state.safetensors");
}

inline std::uint64_t expected_magenta_model_file_size(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    if (name == "mrt2_small.mlxfn") return 455654550ULL;
    if (name == "mrt2_small_state.safetensors") return 8676998ULL;
    if (name == "mrt2_base.mlxfn") return 2771414746ULL;
    if (name == "mrt2_base_state.safetensors") return 16939969ULL;
    return 0;
}

inline bool magenta_file_complete(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    const auto expected = expected_magenta_model_file_size(path);
    if (expected == 0) return true;
    const auto actual = std::filesystem::file_size(path, ec);
    return !ec && actual == expected;
}

inline bool magenta_model_bundle_complete(const std::filesystem::path& checkpoint) {
    if (checkpoint.empty()) return false;
    return magenta_file_complete(checkpoint) &&
           magenta_file_complete(magenta_state_path_for_model(checkpoint));
}

}  // namespace magenta_demo
