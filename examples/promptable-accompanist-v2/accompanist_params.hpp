#pragma once

#include <pulp/state/store.hpp>

#include <cstdint>

namespace pulp::examples::accompanist_v2 {

enum ParamId : state::ParamID {
    kTemperature = 0,
    kTopK,
    kCfgMusicCoCa,
    kCfgNotes,
    kCfgDrums,
    kVolumeDb,
    kFreeze,
    kCaptureSeconds,
    kLoopCrossfadeMs,
    kCount,
};

inline constexpr std::uint32_t kParameterCount = static_cast<std::uint32_t>(kCount);

}  // namespace pulp::examples::accompanist_v2
