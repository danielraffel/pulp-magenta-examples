# Setup — validated recipe (Apple Silicon, macOS 14+)

Every step below was empirically validated end-to-end: `magentart::core` builds and
`hello_mrt2` generated non-silent audio. Follow it exactly — the non-obvious prerequisites
(Metal Toolchain, the MLX bump) are what make MRT2 actually *run* on Xcode 26.x.

## 0. Prerequisites

```bash
# Metal Toolchain — REQUIRED. MLX compiles Metal shaders at build time; the component is NOT
# installed by default even with full Xcode. ~688 MB, one-time.
xcodebuild -downloadComponent MetalToolchain
xcrun metal --version   # should succeed

# uv + a compatible cmake (MRT2 needs cmake >=3.27 and <3.28; a newer system cmake fails)
curl -LsSf https://astral.sh/uv/install.sh | sh
uv venv --python 3.12 && source .venv/bin/activate
uv pip install "cmake<3.28"
```

## 1. Model weights (headless)

The `mrt models` picker needs a TTY; pass the model **NAME** and redirect stdin to run headless:

```bash
uv pip install "magenta-rt[mlx]"
./scripts/install-weights.sh mrt2_small     # or: mrt2_base (Pro/Max only)
# → ~/Documents/Magenta/magenta-rt-v2/{models/mrt2_small/mrt2_small.mlxfn, resources/...}
```

## 2. The MLX / Metal-26 fix (baked into the wrapper)

Magenta pins **MLX `v0.31.1`**, which **builds but aborts at generation** on the Xcode 26.x
Metal Toolchain (`unknown type name 'bfloat16_t' / 'complex64_t'`, `offset_neg_idx` undeclared —
MLX JIT-compiles its Metal kernels at runtime). The fix (MLX PR #3607, merged 2026-06-02):

- `magentart-wrapper/CMakeLists.txt` overrides the MLX FetchContent pin to commit
  **`e9e20fa69184bd38cc0ca12bd9a854c059e59588`** with `GIT_SHALLOW OFF` (so the SHA is reachable).
- If you ever re-pin MLX on an existing build, **delete the stale `_deps/mlx-*`** first — a
  shallow `v0.31.1` clone can't update to an unrelated SHA (`fatal: unable to read tree …`).

Magenta's MLX C++ API (`mlx::core::array`, `ImportedFunction`, `.mlxfn` import/state) is stable
across the bump — no source changes to Magenta are needed.

## 3. Why a wrapper (not `pulp add` on Magenta's repo directly)

Magenta's **root** CMake can't be consumed by FetchContent: it hard-fails off-Apple, builds an
`ALL` npm UI target (`build_mrt2_ui`), and adds all the app/example subdirs. The thin
`magentart-wrapper/` replicates the three deps (MLX-bumped, SentencePiece, TF-Lite + the TF
source-dir pin + the MLX path-with-spaces patch) and pulls in **`core/` only** via
`FetchContent ... SOURCE_SUBDIR core`. `pulp add magenta-realtime-2` therefore points at this
wrapper, exposing just `magentart::core`.

## 4. SDK pinning — float by default

This is a normal `pulp create` project in **floating-SDK mode** (`sdk_version = "latest"`), so it
tracks the latest installed Pulp SDK and never goes stale. Override only to reproduce a bug:
`pulp project pin <version>`.

## Packaging note (for shipped plugins)

MLX produces a `.metallib` that must be placed + (re)signed inside the plugin/`.app` bundle.
Magenta's app-side codesign machinery is **not** inherited by a `magentart::core`-only consumer,
so Pulp's `ship` path must handle metallib placement when packaging these demos.
