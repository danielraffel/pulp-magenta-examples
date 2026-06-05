# pulp-magenta-examples — MRT2 example plugins for Pulp.
# OBJCXX because magentart::core (MLX/Metal) needs the Objective-C++ toolchain.
cmake_minimum_required(VERSION 3.27)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
project(PulpMagentaExamples LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "pulp-magenta-examples is Apple-Silicon/macOS only (MLX/Metal).")
endif()

find_package(Pulp REQUIRED)

# Magenta inference core via the thin wrapper (exposes only magentart::core).
add_subdirectory(magentart-wrapper)

add_subdirectory(examples/promptable-accompanist)
