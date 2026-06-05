# Continuation Effect (E3) — headless prefill → continuation. Plain executable on magentart::core.
add_executable(mrt2-continuation continuation.cpp)
target_link_libraries(mrt2-continuation PRIVATE magentart::core)

set(_m "$ENV{HOME}/Documents/Magenta/magenta-rt-v2/models/mrt2_small/mrt2_small.mlxfn")
set(_r "$ENV{HOME}/Documents/Magenta/magenta-rt-v2/resources")
set(_s "$ENV{HOME}/Documents/Magenta/magenta-rt-v2/resources/spectrostream/spectrostream_encoder.mlxfn")
if(PULP_BUILD_TESTS AND EXISTS "${_m}" AND EXISTS "${_s}")
    add_test(NAME mrt2-continuation
        COMMAND mrt2-continuation "${_m}" "${_r}" "${_s}" "${CMAKE_BINARY_DIR}/continuation.wav")
    set_tests_properties(mrt2-continuation PROPERTIES LABELS "validation;mrt2" TIMEOUT 240)
endif()
