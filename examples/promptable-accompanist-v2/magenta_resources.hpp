#pragma once

// Magenta RealTime 2 shared resources — the model-independent assets the engine
// needs to run: the MusicCoCa text/audio encoders + tokenizer and the
// SpectroStream codec (~1.3 GB total, CC-BY-4.0 Google DeepMind). They are NOT
// per-model, so they live beside the models in the shared Pulp store
// (~/.pulp/magenta/resources/) rather than under any one model's directory.
//
// A plugin-only install (someone who installs just the AU/VST3/CLAP, never the
// legacy ~/Documents/Magenta layout) needs these too — the in-plugin Models
// overlay downloads them on first model install via download_resources().
//
// URLs mirror the HuggingFace repo layout (google/magenta-realtime-2), verified
// to resolve under resources/{musiccoca,spectrostream}/. Sizes are the on-disk
// byte counts, used only to weight the combined download progress bar.

#include <pulp/runtime/model_registry.hpp>  // resolve_checkpoint_url
#include <pulp/runtime/model_download.hpp>  // download_file, DownloadRequest
#include <pulp/runtime/model_store.hpp>     // resolve_pulp_home
#include <pulp/runtime/async_stream.hpp>    // CancellationToken

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace magenta_demo {

struct ResourceFile {
    const char* rel_path;        // relative to resources/, e.g. "musiccoca/spm.model"
    const char* checkpoint_ref;  // hf:// ref (resolve_checkpoint_url → https)
    std::uint64_t size_bytes;
};

// The 10 files the engine loads via init_assets(resources_dir, "musiccoca").
inline const std::vector<ResourceFile>& magenta_resource_files() {
    static const std::vector<ResourceFile> files = {
        {"musiccoca/audio_preprocessor.tflite",
         "hf://google/magenta-realtime-2/resources/musiccoca/audio_preprocessor.tflite", 8729640ULL},
        {"musiccoca/mapper.tflite",
         "hf://google/magenta-realtime-2/resources/musiccoca/mapper.tflite", 86166664ULL},
        {"musiccoca/music_encoder.tflite",
         "hf://google/magenta-realtime-2/resources/musiccoca/music_encoder.tflite", 370935584ULL},
        {"musiccoca/pretrained_vector_quantizer.tflite",
         "hf://google/magenta-realtime-2/resources/musiccoca/pretrained_vector_quantizer.tflite",
         72422108ULL},
        {"musiccoca/spm.model",
         "hf://google/magenta-realtime-2/resources/musiccoca/spm.model", 517448ULL},
        {"musiccoca/text_encoder.tflite",
         "hf://google/magenta-realtime-2/resources/musiccoca/text_encoder.tflite", 418674324ULL},
        {"spectrostream/decoder.safetensors",
         "hf://google/magenta-realtime-2/resources/spectrostream/decoder.safetensors", 209853216ULL},
        {"spectrostream/encoder.safetensors",
         "hf://google/magenta-realtime-2/resources/spectrostream/encoder.safetensors", 37013392ULL},
        {"spectrostream/quantizer.safetensors",
         "hf://google/magenta-realtime-2/resources/spectrostream/quantizer.safetensors", 67108984ULL},
        {"spectrostream/spectrostream_encoder.mlxfn",
         "hf://google/magenta-realtime-2/resources/spectrostream/spectrostream_encoder.mlxfn",
         104319983ULL},
    };
    return files;
}

inline std::filesystem::path shared_resources_dir() {
    return pulp::runtime::resolve_pulp_home() / "magenta" / "resources";
}

inline std::filesystem::path legacy_resources_dir() {
    return std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") /
           "Documents/Magenta/magenta-rt-v2/resources";
}

// All required resource files present on disk at this resources root?
inline bool resources_complete_at(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& f : magenta_resource_files()) {
        const auto path = dir / f.rel_path;
        if (!std::filesystem::exists(path, ec)) return false;
        ec.clear();
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size != f.size_bytes) return false;
    }
    return true;
}

// Runtime availability is intentionally looser than download completeness.
// The size manifest is only a repair/download hint; if all resource files are
// present, let MLX/MusicCoCa init_assets() decide whether they are usable.
inline bool resources_available_at(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& f : magenta_resource_files()) {
        const auto path = dir / f.rel_path;
        if (!std::filesystem::exists(path, ec)) return false;
        ec.clear();
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0) return false;
    }
    return true;
}

// All required resource files present on disk in the shared store?
inline bool shared_resources_complete() {
    return resources_complete_at(shared_resources_dir());
}

inline bool legacy_resources_complete() {
    return resources_complete_at(legacy_resources_dir());
}

inline bool runtime_resources_complete() {
    return shared_resources_complete() || legacy_resources_complete();
}

inline bool shared_resources_available() {
    return resources_available_at(shared_resources_dir());
}

inline bool legacy_resources_available() {
    return resources_available_at(legacy_resources_dir());
}

inline bool runtime_resources_available() {
    return shared_resources_available() || legacy_resources_available();
}

inline std::uint64_t magenta_resources_total_bytes() {
    std::uint64_t total = 0;
    for (const auto& f : magenta_resource_files()) total += f.size_bytes;
    return total;
}

// Download any missing resource files into the shared store, preserving the
// musiccoca/ + spectrostream/ subdirectory layout init_assets expects. Streams +
// resumes (download_file). `on_file_progress(done_bytes, total_bytes)` reports
// aggregate progress across all files; returning false (or a cancelled token)
// aborts and keeps partials for a later resume. Returns true once every file is
// present. No sha256 is enforced (the manifest ships no digests for resources);
// a truncated file is caught on the next run because its size check / engine load
// fails, and resume continues the .part.
inline bool download_resources(
    const std::function<bool(std::uint64_t done, std::uint64_t total)>& on_file_progress,
    const pulp::runtime::CancellationToken* cancel = nullptr) {
    const auto dir = shared_resources_dir();
    const std::uint64_t total = magenta_resources_total_bytes();
    std::uint64_t completed_bytes = 0;

    for (const auto& f : magenta_resource_files()) {
        const auto dest = dir / f.rel_path;
        std::error_code ec;
        if (std::filesystem::exists(dest, ec)) {  // already have it
            ec.clear();
            const auto existing_size = std::filesystem::file_size(dest, ec);
            if (ec || existing_size != f.size_bytes) {
                std::filesystem::remove(dest, ec);
                ec.clear();
            } else {
                completed_bytes += f.size_bytes;
                if (on_file_progress && !on_file_progress(completed_bytes, total)) return false;
                continue;
            }
        }
        std::filesystem::create_directories(dest.parent_path(), ec);

        const std::uint64_t base = completed_bytes;
        pulp::runtime::DownloadRequest req;
        req.url = pulp::runtime::resolve_checkpoint_url(f.checkpoint_ref);
        req.dest = dest;
        req.resume = true;

        const auto res = pulp::runtime::download_file(
            req,
            [&](const pulp::runtime::DownloadProgress& p) {
                if (on_file_progress && !on_file_progress(base + p.downloaded, total)) return false;
                return true;
            },
            cancel);
        if (!res.ok) return false;
        completed_bytes = base + f.size_bytes;
    }
    return true;
}

}  // namespace magenta_demo
