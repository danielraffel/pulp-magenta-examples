# magentart-core-wrapper

Thin CMake wrapper that exposes **only** `magentart::core` to a downstream Pulp consumer
(`pulp add magenta-realtime-2`). It exists because Magenta's *root* CMake can't be FetchContent'd
(hard-fails off-Apple, builds an npm UI `ALL` target + all app/example subdirs).

What it does:
1. Guards Apple + cmake≥3.27 + OBJCXX + macOS 15 + C++20/Abseil.
2. Replicates Magenta's three deps — **MLX pinned to `e9e20fa`** (the Metal-26 fix, `GIT_SHALLOW OFF`),
   SentencePiece `v0.2.0`, TensorFlow-Lite `v2.21.0` (+ the TF source-dir pin + MLX path patch).
3. Pulls in `core/` only via `FetchContent ... SOURCE_SUBDIR core`.

Downstream then links `magentart::core`. See `../docs/SETUP.md` for the validated recipe.
