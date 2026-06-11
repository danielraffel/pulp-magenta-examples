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

For any DMG/share build, run the package-isolation DMG script instead of packaging the build-tree
app directly:

```bash
./scripts/build-v2-test-dmg.sh
```

That script runs the Release smoke, builds the V2 Pulp format bundles, creates a DMG, mounts it,
temporarily hides the CMake build directory, launches the app from the mounted DMG in hidden
package-audit mode, and fails on dyld/missing-runtime/default-metallib/model-load errors. This is
the guard against builds that only work because generated dependency artifacts are still available
in the local build tree. When a Developer ID identity is available, the script signs the staged app
and DMG; set `PULP_MAGENTA_V2_NOTARIZE=1` with Apple notary credentials for a Gatekeeper-ready share
build.

For V2 share builds, include the native Pulp plug-in formats in the DMG via a customizable signed
installer package: Standalone -> `/Applications`, VST3 -> `/Library/Audio/Plug-Ins/VST3`, AUv2 ->
`/Library/Audio/Plug-Ins/Components`, and CLAP -> `/Library/Audio/Plug-Ins/CLAP`. The standalone app
can still sit directly in the DMG for drag-run testing. Do not introduce JUCE build files,
dependencies, or example code for this packaging path.

## Gotchas (validated)

- The wrapper **pins MLX to `e9e20fa`** (Metal-26 fix). If you re-pin MLX on an existing build,
  delete `_deps/mlx-*` first (a shallow clone can't update to an unrelated SHA).
- `mlx.metallib` is a required runtime artifact, not a normal dylib. Local builds can appear to
  work because MLX falls back to a compiled-in build-tree `METAL_PATH`; shipped apps must copy
  `mlx.metallib` next to each generated executable or into an MLX-supported Resources location,
  make it readable, and re-sign bundles after every clean copy into a staging root. Treat it as a
  resource for packaging, but remember that macOS `codesign` treats files under `Contents/MacOS`
  as nested code. If the packaging script copies bundles with `ditto --noextattr`, it strips the
  nested signature xattrs from `mlx.metallib`; the copied app/plugin bundle must be re-signed with
  `codesign --deep` before notarization. This applies both to the DMG-staged app and to every
  installer component root. Missing this file shows up on other Macs as `Failed to load the default
  metallib. library not found`; stripped nested signatures show up in notary logs as invalid app
  binaries inside component packages.
- Package audits should use `PULP_STANDALONE_PACKAGE_AUDIT=1` instead of relying on a visible app
  window. The audit should wait for the packaged app to run hidden, initialize audio/model state,
  and exit cleanly; this avoids screen flashes and catches packaging failures without UI timing
  assumptions.
- Hidden package audits must mute generated output by default. They may open the audio device to
  exercise the real standalone path, but do not let test audio reach speakers unless an explicit
  audible-smoke env var is set and you have warned the user before launching it.
- A Developer-ID-signed but unnotarized DMG can still trigger Gatekeeper warnings for other users.
  For friend/share builds, sign the app/plugin bundles with Developer ID Application, sign
  installer packages with Developer ID Installer, notarize and staple both `.pkg` and `.dmg`, then
  run the primary signature `spctl` check before calling it releasable.
- In Codex/non-interactive shells, direct Developer ID signing can fail with
  `errSecInternalComponent` or `User interaction is not allowed` even when the same Terminal
  session works. Prefer the V2 GUI LaunchAgent release-builder path when available: run its signing
  probe first, then trigger the build with `PULP_STANDALONE_PACKAGE_AUDIT=0` unless the user
  explicitly opts into an audit that launches CoreAudio. If the probe hangs at `pkgbuild`, the
  Installer identity still needs keychain ACL/partition-list setup on that Mac.
- If release signing fails with `errSecInternalComponent`, check for duplicate Developer ID
  identities in multiple user keychains. Unlock and set the Apple/codesign partition list on every
  matching keychain before rerunning the release script, because `codesign` can select a locked
  duplicate. For V2, pin the scripts to the intended keychain with
  `PULP_MAGENTA_V2_CODESIGN_KEYCHAIN` and `PULP_MAGENTA_V2_INSTALLER_KEYCHAIN` when one duplicate
  keychain has an unknown or stale password.
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
- Treat the active model id as a preference, not proof of a loadable model. Before every reload,
  resolve a complete and hardware-compatible bundle; stale active metadata, deleted models,
  partial downloads, and M1-family `mrt2_base` selections must fall back to Small or show the
  download-model gate before MLX gets a path.
- Treat a cleared prompt as **masked MusicCoCa tokens**, not `set_text_prompt("")`. Empty text can
  still be encoded as a prompt/fallback in Magenta; the masked-token API is the deliberate
  no-text-conditioning path. Debounce live prompt edits so replacing text (`cello` -> `violin`)
  does not briefly apply a transient empty/no-prompt state mid-typing.
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
