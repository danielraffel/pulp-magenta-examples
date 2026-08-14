#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-"$repo_root/build-model-status"}"
jobs="${PULP_JOBS:-$(sysctl -n hw.ncpu)}"
deploy_target="${CMAKE_OSX_DEPLOYMENT_TARGET:-15.0}"

cmake -S "$repo_root" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$deploy_target"

cache="$build_dir/CMakeCache.txt"
grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$cache"
grep -q '^CMAKE_OSX_DEPLOYMENT_TARGET:STRING=15\.0$' "$cache"

cmake --build "$build_dir" \
  --target promptable-accompanist-v2-test PromptableAccompanistV2_Standalone \
  -j"$jobs"

standalone_app="$build_dir/examples/promptable-accompanist-v2/PromptableAccompanistV2.app"
standalone_metallib="$standalone_app/Contents/MacOS/mlx.metallib"
build_metallib="$build_dir/_deps/mlx-build/mlx/backend/metal/kernels/mlx.metallib"
test -s "$build_metallib"
test -s "$standalone_metallib"
cmp -s "$build_metallib" "$standalone_metallib"
if command -v codesign >/dev/null 2>&1; then
  codesign --verify --deep --strict "$standalone_app"
fi

test_flags="$(find "$build_dir/examples/promptable-accompanist-v2" \
  -path '*/CMakeFiles/promptable-accompanist-v2-test.dir/flags.make' \
  -print -quit)"
standalone_flags="$(find "$build_dir/examples/promptable-accompanist-v2" \
  -path '*/CMakeFiles/PromptableAccompanistV2_Standalone.dir/flags.make' \
  -print -quit)"

for flags in "$test_flags" "$standalone_flags"; do
  test -n "$flags"
  grep -q -- '-O3' "$flags"
  grep -q -- '-DNDEBUG' "$flags"
  grep -q -- '-mmacosx-version-min=15.0' "$flags"
done

"$build_dir/examples/promptable-accompanist-v2/promptable-accompanist-v2-test"

PULP_MAGENTA_V2_RUN_MODEL_SMOKE=1 \
PULP_MAGENTA_V2_DEBUG="${PULP_MAGENTA_V2_DEBUG:-1}" \
  "$build_dir/examples/promptable-accompanist-v2/promptable-accompanist-v2-test"

echo "OK: PromptableAccompanistV2 Release build, hot-reload smoke, and bundle guards passed"
