# magentart-core-wrapper — exposes ONLY `magentart::core` to downstream
# Pulp consumers (pulp add → FetchContent). Recreates the dependency graph
# that magenta-realtime's ROOT CMake builds, then pulls in core/ via
# SOURCE_SUBDIR so the root's npm `build_mrt2_ui ALL` target and the
# app/example subdirectories are never configured.
#
# This is the package `pulp add magenta-realtime-2` points at (NOT the
# upstream root, which hard-fails off-Apple and requires npm).
cmake_minimum_required(VERSION 3.27)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
project(magentart_core_wrapper LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
  message(FATAL_ERROR "magentart::core is macOS/Apple-Silicon only (MLX/Metal + Apple frameworks).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(ABSL_INTERNAL_AT_LEAST_CXX20 ON CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON)

# Pin the Magenta revision the demos are validated against.
set(MAGENTART_GIT_TAG "main" CACHE STRING "magenta-realtime git ref")

include(FetchContent)

# --- MLX v0.31.1 (replicated from upstream root) ------------------------------
FetchContent_Declare(mlx
  GIT_REPOSITORY https://github.com/ml-explore/mlx.git
  GIT_TAG e9e20fa69184bd38cc0ca12bd9a854c059e59588 GIT_SHALLOW OFF)  # MLX #3607 — Metal-26 fix (Phase 0)
foreach(opt TESTS EXAMPLES BENCHMARKS PYTHON_BINDINGS PYTHON_STUBS GGUF CUDA)
  set(MLX_BUILD_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_MakeAvailable(mlx)
# Upstream MLX path-with-spaces patch (root CMake lines 79-86; drop once ml-explore/mlx#3607 lands)
file(READ "${mlx_SOURCE_DIR}/mlx/backend/metal/make_compiled_preamble.sh" _MLX_C)
string(REPLACE "declare -a HDRS_LIST=($HDRS)"
  "declare -a HDRS_LIST=(); while read -r dots path; do [ -n \"$dots\" ] && HDRS_LIST+=(\"$dots\" \"$path\"); done <<< \"$HDRS\""
  _MLX_C "${_MLX_C}")
file(WRITE "${mlx_SOURCE_DIR}/mlx/backend/metal/make_compiled_preamble.sh" "${_MLX_C}")

# --- SentencePiece v0.2.0 -----------------------------------------------------
FetchContent_Declare(sentencepiece
  GIT_REPOSITORY https://github.com/google/sentencepiece.git
  GIT_TAG v0.2.0 GIT_SHALLOW ON)
set(SPM_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sentencepiece)

# --- TensorFlow Lite v2.21.0 --------------------------------------------------
FetchContent_Declare(tensorflow-lite
  GIT_REPOSITORY https://github.com/tensorflow/tensorflow.git
  GIT_TAG v2.21.0 SOURCE_SUBDIR tensorflow/lite GIT_SHALLOW ON)
set(TFLITE_ENABLE_XNNPACK OFF CACHE BOOL "" FORCE)
set(TFLITE_ENABLE_GPU OFF CACHE BOOL "" FORCE)
set(TFLITE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_TENSORFLOW "${CMAKE_BINARY_DIR}/_deps/tensorflow-lite-src" CACHE PATH "" FORCE)
FetchContent_MakeAvailable(tensorflow-lite)

# --- magentart::core ONLY (SOURCE_SUBDIR core skips root + apps + npm UI) ------
FetchContent_Declare(magenta_realtime
  GIT_REPOSITORY https://github.com/magenta/magenta-realtime.git
  GIT_TAG ${MAGENTART_GIT_TAG}
  SOURCE_SUBDIR core
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(magenta_realtime)
# Downstream now links `magentart::core`.
