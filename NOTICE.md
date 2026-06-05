# NOTICE — third-party attribution

This project (`pulp-magenta-examples`) is MIT-licensed. It builds on the following, whose
attribution travels with anything you distribute that is built from this repo.

## Magenta RealTime 2 — code (Apache-2.0)

> Copyright 2026 Google LLC
> Licensed under the Apache License, Version 2.0.
> https://github.com/magenta/magenta-realtime — `magentart::core`

Consumed (unmodified) via the thin `magentart-wrapper/`, which FetchContents
`magenta-realtime` at a pinned revision and the MLX dependency at commit `e9e20fa`.

## Magenta RealTime 2 — model weights (CC-BY-4.0)

> Magenta RealTime 2 model weights © Google DeepMind, licensed
> [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/).
> Source: https://huggingface.co/google/magenta-realtime-2

Weights are **downloaded by the user** (`scripts/install-weights.sh`) and are **not**
redistributed in this repository. Any application that ships generated audio or bundles the
weights must carry this CC-BY-4.0 attribution (about-box, credits, and package metadata).

## Transitive build dependencies (via the wrapper, Apache-2.0 / BSD / MIT)

- MLX (MIT) — https://github.com/ml-explore/mlx (pinned `e9e20fa`)
- SentencePiece (Apache-2.0) — https://github.com/google/sentencepiece
- TensorFlow Lite (Apache-2.0) — https://github.com/tensorflow/tensorflow
