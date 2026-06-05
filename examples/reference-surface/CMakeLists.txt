# Reference-Surface validator (Phase 7 headless core) — proves the MRT2 control surface the
# reference apps (Jam/Collider/all-in-one) expose. Plain executable on magentart::core.
add_executable(mrt2-reference-surface reference_surface.cpp)
target_link_libraries(mrt2-reference-surface PRIVATE magentart::core)

set(_m "$ENV{HOME}/Documents/Magenta/magenta-rt-v2/models/mrt2_small/mrt2_small.mlxfn")
set(_r "$ENV{HOME}/Documents/Magenta/magenta-rt-v2/resources")
if(PULP_BUILD_TESTS AND EXISTS "${_m}")
    add_test(NAME mrt2-reference-surface COMMAND mrt2-reference-surface "${_m}" "${_r}")
    set_tests_properties(mrt2-reference-surface PROPERTIES LABELS "validation;mrt2" TIMEOUT 240)
endif()
