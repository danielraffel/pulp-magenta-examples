# pulp-magenta-examples

Cross-platform [Pulp](https://github.com/danielraffel/pulp) example plugins built on
**Google Magenta RealTime 2 (MRT2)** — the on-device real-time music model. These demos turn
Magenta's single AUv3 into **VST3 + AU + CLAP + standalone** from one `Processor`, install the
weights with one command, and (flagship) let an AI agent conduct a live MRT2 instance over MCP.

> **Optional & external by design.** Nothing here lives in Pulp's core. Magenta is pulled in as
> a third-party **package** (`pulp add magenta-realtime-2`) via the thin
> [`magentart-wrapper/`](magentart-wrapper/). This project **floats to the latest Pulp SDK** so
> it never goes stale — see [docs/SETUP.md](docs/SETUP.md).

## Two ways to use these

**A. Just play with the prebuilt plugins** — no build, no toolchain.
Download the signed + notarized installer from the
[latest release](https://github.com/danielraffel/pulp-magenta-examples/releases/latest)
(`PromptableAccompanist-<version>.pkg`) and run it. Pick which formats to install
(AU / VST3 / CLAP / Standalone). On first launch — the standalone app, or the plugin loaded on an
instrument track in your DAW — open **⚙ Settings → Models** and download a model; the shared
resources come down with it. Nothing else to set up. (Apple Silicon only.)

**B. Build & develop with Magenta in Pulp** — you have the Pulp SDK installed and want to write
your own Magenta-powered plugin, or hack on E1–E3. The examples build against your installed SDK
via `find_package(Pulp)`. See [Build it yourself](#build-it-yourself) below — and note the
model-manager (browse / download / set-default a model) is a **core Pulp SDK feature**
(`<pulp/runtime/model_store.hpp>` + `<pulp/view/model_manager_view.hpp>`), so any plugin you write
can offer the same picker — Magenta is just the first package to use it.

## Requirements (Apple Silicon only)

- Apple Silicon Mac (M-series), macOS 15+.
- Xcode + the **Metal Toolchain** component: `xcodebuild -downloadComponent MetalToolchain`
  (≈688 MB — required; MLX compiles Metal shaders at build time and it is **not** installed by
  default even with full Xcode).
- `uv` and a `cmake<3.28` (MRT2's build pins cmake ≥3.27, <3.28).
- The `mrt` CLI for weights: `uv pip install "magenta-rt[mlx]"`.

`mrt2_small` (230M) runs real-time on any Apple Silicon; `mrt2_base` (2.4B) needs a Pro/Max.

## Build it yourself

You have the Pulp SDK installed, so the examples build straight against it.

```bash
git clone https://github.com/danielraffel/pulp-magenta-examples
cd pulp-magenta-examples

# 1. Bring Magenta in via Pulp's package manager (resolves the local registry entry)
pulp add magenta-realtime-2

# 2. Build the demos cross-format (AU / VST3 / CLAP / Standalone) — fetches + builds
#    MLX / TensorFlow-Lite / the magentart wrapper on the first configure
pulp build

# 3. Validate (auval / clap-validator)
pulp validate
```

**Getting a model.** For the **E1 Promptable Accompanist** you don't need to pre-install weights —
launch it and use **⚙ Settings → Models** to download one; the shared resources come with it, and
swapping models hot-reloads the running engine (no restart). The headless demos
(**E2 Agent Session**, **E3 Continuation**) have no UI, so install weights up front:

```bash
uv pip install "magenta-rt[mlx]"
./scripts/install-weights.sh mrt2_small      # or mrt2_base (Pro/Max only)
```

### What this installs / downloads

| When | What | Where | Size |
|------|------|-------|------|
| Prereqs (one-time) | Xcode **Metal Toolchain**, `uv`, `cmake<3.28` | system | ~688 MB + tooling |
| First `pulp build` | MLX, TensorFlow-Lite, the magentart wrapper (FetchContent) | `build/_deps/` | a few GB, cached |
| A model (in-app **or** `install-weights.sh`) | `mrt2_small` (~440 MB) or `mrt2_base` (~2.6 GB) | in-app → shared store `~/.pulp/magenta`; script → `~/Documents/Magenta` | 0.4–2.6 GB |
| First model, in-app | shared resources (MusicCoCa + SpectroStream) | shared store `~/.pulp/magenta` | ~1.3 GB |
| Install step (`pulp build --install` or the `.pkg`) | the plugins / app | `~/Library/Audio/Plug-Ins/{Components,VST3,CLAP}`, `/Applications` | — |

Model weights are **CC-BY-4.0 (Google DeepMind)** — downloaded by you on demand, never bundled or
redistributed here.

## What's here / planned

| Demo | Kind | Status |
|------|------|--------|
| Promptable Accompanist (E1) | MIDI-conditioned generative instrument | **working, cross-format** — **AU (auval PASSES) + VST3 + CLAP + Standalone(.app)** — installable in Logic; generates from prompt + takes MIDI. **GPU-native editor** (faders + prompt) by default, with an in-plugin **⚙ Settings → Models** picker (download / set-default / hot-reload) + auto resource fetch. Optional WebView editor via `-DPROMPTABLE_WEBVIEW_UI=ON` |
| Agent-Conducted Session (E2) | flagship — `.pulpset` of agent steers (numeric) replayed to audio | **working** — headless conductor replays commands → non-silent audio; conducted build measurably raises energy (seg0→seg2). Live MCP-agent layer is the demo extension |
| Continuation Effect (E3) | seed → prefill → generated continuation | **working** — token-prefill (lossless) branch; continuation coherent with seed, non-silent |
| WebView UI (Phase 7) | custom editor — prompt box + control sliders, bridged to the engine | **working** — opt-in (`-DPROMPTABLE_WEBVIEW_UI=ON`); auval SUCCEEDED with a Cocoa view → shows in Logic; reference-surface (4/4) proves the control surface |

See the delivery plan (`planning/2026-06-05-magenta-rt2-delivery-plan.md` in the private
pulp-planning repo) for the phased roadmap.

## Freeze, loop & drag-to-DAW (Promptable Accompanist)

The accompanist is a *live* generator, but you often want to keep a passage. The
**Freeze** control (bottom row of the GPU-native editor) does two things:

- **Tap Freeze** — captures the last few seconds of generated audio into a
  **seamless loop** and holds it (the generator stops advancing; the loop plays
  back). Tap again to unfreeze and resume live generation. Two faders shape the
  capture:
  - **Capture** (`kCaptureSeconds`, 0.25–8 s, default 2 s) — how much recent
    audio is frozen into the loop.
  - **Loop XFade** (`kLoopCrossfadeMs`, 0–100 ms, default 30 ms) — the equal-power
    crossfade applied at the loop seam so it repeats without a click.
- **Drag Freeze** — press and drag the Freeze button (past a small threshold)
  to start a **native outbound file drag** of the frozen loop as a `.wav`. Drop
  it straight onto your DAW's timeline, an audio track, or the Finder. This rides
  the core SDK's `View::start_file_drag` / `FileDragRequest` outbound-drag path
  (NSDraggingSession on macOS), so it behaves like dragging any audio file.

Freeze, Capture, and Loop XFade are **host-automatable parameters**: editor edits
record as automation and follow playback (gesture brackets + reverse-sync). Note
the generator itself is free-running — Freeze does **not** tempo-lock the loop to
the host grid; host-tempo / bar-aligned looping is tracked as a future
enhancement ([pulp #4148](https://github.com/danielraffel/pulp/issues/4148)).

> Editor text input (the prompt field) and DAW keyboard etiquette — focus
> highlight + caret, Space/R handing back to the host transport when you leave
> the field, Escape/Tab/Return to exit — are handled by the Pulp SDK
> (`core/view`), so they require an SDK that includes those fixes. This repo
> builds in **floating-SDK mode** (`sdk_version = "latest"` in `pulp.toml`), so a
> normal build picks them up automatically; run `pulp upgrade` if your installed
> SDK predates them.

## Honest limitations

- MRT2 is **generative with ~200 ms latency** — MIDI *steers* density/region/harmony; it is **not**
  a deterministic synth voice that "plays the notes you press."
- Requires the **MLX `e9e20fa` bump** (the wrapper pins it) to run on the Xcode 26.x Metal
  Toolchain — Magenta's own `v0.31.1` pin aborts at generation. See [docs/SETUP.md](docs/SETUP.md).

## License & attribution

This repo: MIT. Magenta RealTime 2 **code** is Apache-2.0 and the **model weights** are
CC-BY-4.0 (Google DeepMind) — see [NOTICE.md](NOTICE.md). Weights are downloaded by you, never
redistributed here.
