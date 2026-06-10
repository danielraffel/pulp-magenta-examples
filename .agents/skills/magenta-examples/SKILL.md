---
name: magenta-examples
description: Bring Magenta RealTime 2 into a Pulp project and build/validate the MRT2 example plugins (cross-format, Apple Silicon). Handles the Metal Toolchain + MLX-26 prerequisites, headless weights, and `pulp add magenta-realtime-2`.
requires:
  scripts:
    - scripts/install-weights.sh
    - scripts/smoke-v2-hot-reload.sh
    - magentart-wrapper/CMakeLists.txt
    - tools/packages/registry.json
---

# Magenta examples — bring-it-in skill

Use when a user wants to run the MRT2 example plugins (Promptable Accompanist, Agent-Conducted
Session, Continuation Effect) in Pulp, or to add `magentart::core` to a Pulp project.

## Preconditions (check first, in order)

1. **Apple Silicon + macOS 15+.** Otherwise stop — MRT2 is Metal/MLX-only.
2. **Metal Toolchain installed:** `xcrun metal --version` must succeed. If not:
   `xcodebuild -downloadComponent MetalToolchain` (~688 MB, one-time). This is the #1 gotcha —
   the build links fine without it but **aborts at generation**.
3. **`mrt` CLI:** `command -v mrt` — else `uv pip install "magenta-rt[mlx]"`.
4. **cmake `<3.28`** in the active env (MRT2 pins ≥3.27, <3.28).

## Steps

```bash
# weights (headless — pass the NAME, the default picker needs a TTY)
./scripts/install-weights.sh mrt2_small        # mrt2_base needs a Pro/Max

# bring Magenta in as a package (resolves the local tools/packages/registry.json entry,
# which points at magentart-wrapper/ — NOT Magenta's root CMake)
pulp add magenta-realtime-2

pulp build
pulp validate            # auval / clap-validator on the built formats
```

## Promptable Accompanist V2 hot-reload validation

For any change touching V2 model loading, model downloads, resource detection, prompt encoding,
audio continuity, or freeze-loop behavior, run the codified Release smoke:

```bash
./scripts/smoke-v2-hot-reload.sh
```

This verifies Release config/flags, builds the V2 test and standalone, runs the normal contract,
then runs the real-model smoke (`PULP_MAGENTA_V2_RUN_MODEL_SMOKE=1`) that checks generated audio,
freeze/release, hot-switch to another installed model, and rejected incomplete-model reloads that
must preserve the previous model and audible output.

## Gotchas (validated)

- The wrapper **pins MLX to `e9e20fa`** (Metal-26 fix). If you re-pin MLX on an existing build,
  delete `_deps/mlx-*` first (a shallow clone can't update to an unrelated SHA).
- MRT2 is **generative, ~200 ms latency** — MIDI steers; it does not deterministically play notes.
  Set that expectation in any UX copy.
- Keep **all MLXEngine mutation on one worker thread**. MLX streams/Metal command encoders are
  thread-local, and touching `MLXEngine` from the audio/UI thread during load or generation can
  make valid models go silent.
- For live model switches, do **not** call `MLXEngine::unload()` before `load_model()`. In this
  wrapper, `unload()` also drops the shared MusicCoCa/TFLite encoder assets loaded by
  `init_assets()`, so the next prompt encode can fail with "model encoders failed to start" even
  though the checkpoint itself loaded.
- During hot reload, keep the previous model/audio ring alive until the replacement model is fully
  loaded, encoded, and primed. If a requested model bundle or resource set is incomplete, report
  that status but preserve the current loaded model so generated audio continues.
- Treat model/resource install checks as **bundle completeness checks**, not existence checks:
  weights, `_state.safetensors`, and shared resources must all be present with expected byte sizes
  before an install is "available".
- Add a generated-audio smoke for model work: load a real model, assert non-silent output, switch to
  another installed model, assert output resumes, then request an intentionally incomplete model and
  assert the previous model path and audible output survive.
- Validate plugins with Pulp's test harness / `pulp validate` before installing to system folders.
- CC-BY-4.0 weight attribution must travel into any shipped bundle (see `NOTICE.md`).

## Update rule

When you discover a new MRT2/Pulp integration gotcha (a build prereq, a wrapper quirk, a
format-validation trap), add it here — this skill is the single source of truth for getting the
demos running.
