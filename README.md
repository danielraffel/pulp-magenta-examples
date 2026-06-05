# pulp-magenta-examples

Cross-platform [Pulp](https://github.com/danielraffel/pulp) example plugins built on
**Google Magenta RealTime 2 (MRT2)** — the on-device real-time music model. These demos turn
Magenta's single AUv3 into **VST3 + AU + CLAP + standalone** from one `Processor`, install the
weights with one command, and (flagship) let an AI agent conduct a live MRT2 instance over MCP.

> **Optional & external by design.** Nothing here lives in Pulp's core. Magenta is pulled in as
> a third-party **package** (`pulp add magenta-realtime-2`) via the thin
> [`magentart-wrapper/`](magentart-wrapper/). This project **floats to the latest Pulp SDK** so
> it never goes stale — see [docs/SETUP.md](docs/SETUP.md).

## Requirements (Apple Silicon only)

- Apple Silicon Mac (M-series), macOS 14+.
- Xcode + the **Metal Toolchain** component: `xcodebuild -downloadComponent MetalToolchain`
  (≈688 MB — required; MLX compiles Metal shaders at build time and it is **not** installed by
  default even with full Xcode).
- `uv` and a `cmake<3.28` (MRT2's build pins cmake ≥3.27, <3.28).
- The `mrt` CLI for weights: `uv pip install "magenta-rt[mlx]"`.

`mrt2_small` (230M) runs real-time on any Apple Silicon; `mrt2_base` (2.4B) needs a Pro/Max.

## Quickstart

```bash
# 1. Install the model weights (headless; pass the NAME to skip the TTY picker)
./scripts/install-weights.sh mrt2_small

# 2. Bring Magenta in via Pulp's package manager (resolves the local registry entry)
pulp add magenta-realtime-2

# 3. Build a demo (cross-format)
pulp build

# 4. Validate with the format validators (auval / clap-validator)
pulp validate
```

## What's here / planned

| Demo | Kind | Status |
|------|------|--------|
| Promptable Accompanist (E1) | MIDI-conditioned generative instrument | **working** — CLAP builds + dlopens, links `magentart::core`, smoke test green (VST3/AU additive) |
| Agent-Conducted Session (E2) | flagship — `.pulpset` of agent steers (numeric) replayed to audio | **working** — headless conductor replays commands → non-silent audio; conducted build measurably raises energy (seg0→seg2). Live MCP-agent layer is the demo extension |
| Continuation Effect (E3) | audio-in → prefill → generated continuation | planned |
| Reference-suite clone (Phase 7) | Jam + Collider + all-in-one, cross-format | planned |

See the delivery plan (`planning/2026-06-05-magenta-rt2-delivery-plan.md` in the private
pulp-planning repo) for the phased roadmap.

## Honest limitations

- MRT2 is **generative with ~200 ms latency** — MIDI *steers* density/region/harmony; it is **not**
  a deterministic synth voice that "plays the notes you press."
- Requires the **MLX `e9e20fa` bump** (the wrapper pins it) to run on the Xcode 26.x Metal
  Toolchain — Magenta's own `v0.31.1` pin aborts at generation. See [docs/SETUP.md](docs/SETUP.md).

## License & attribution

This repo: MIT. Magenta RealTime 2 **code** is Apache-2.0 and the **model weights** are
CC-BY-4.0 (Google DeepMind) — see [NOTICE.md](NOTICE.md). Weights are downloaded by you, never
redistributed here.
