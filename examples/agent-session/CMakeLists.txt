# Agent-Conducted Session (E2) — headless conductor / .pulpset replay.
# Plain executable: links only magentart::core (no plugin scaffolding). The same numeric
# command stream a live agent sends via pulp_inspect_set_param (G1) is replayed here for a
# range/property regression — demo == test.
add_executable(mrt2-conductor conductor.cpp)
target_link_libraries(mrt2-conductor PRIVATE magentart::core)

# Harness test: replay the sample session against the installed model; skip if weights absent.
set(_mrt2_model "$ENV{HOME}/Documents/Magenta/magenta-rt-v2/models/mrt2_small/mrt2_small.mlxfn")
set(_mrt2_res   "$ENV{HOME}/Documents/Magenta/magenta-rt-v2/resources")
if(PULP_BUILD_TESTS AND EXISTS "${_mrt2_model}" AND EXISTS "${_mrt2_res}")
    add_test(NAME mrt2-conductor-replay
        COMMAND mrt2-conductor "${_mrt2_model}" "${_mrt2_res}"
                "${CMAKE_CURRENT_SOURCE_DIR}/session.pulpset" "${CMAKE_BINARY_DIR}/session.wav")
    set_tests_properties(mrt2-conductor-replay PROPERTIES LABELS "validation;mrt2" TIMEOUT 240)
endif()
