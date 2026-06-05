#!/usr/bin/env bash
# Headless MRT2 weights install (validated). The default `mrt models` picker
# needs a TTY; pass the model NAME + redirect stdin to run non-interactively.
set -euo pipefail
MODEL="${1:-mrt2_small}"   # mrt2_small (any M-series) | mrt2_base (Pro/Max)
command -v mrt >/dev/null || { echo "Install the mrt CLI first: uv pip install 'magenta-rt[mlx]'"; exit 1; }
mrt models init --source hf </dev/null
mrt models download "$MODEL" --source hf </dev/null
echo "Installed $MODEL + resources under ~/Documents/Magenta/magenta-rt-v2"
