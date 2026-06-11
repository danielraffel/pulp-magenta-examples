# Promptable Accompanist V2 — Magenta RT generation with a simple freeze/loop sampler lane.
#
# This sibling example keeps E1 stable while proving the sampler/looper primitives in a
# real generated-audio workflow: capture generated stereo, publish a held sample off the
# audio thread, and loop it from the audio callback with start/stop fades.
pulp_add_plugin(PromptableAccompanistV2
    FORMATS           CLAP VST3 AU AUv3 Standalone
    PLUGIN_NAME       "PromptableAccompanistV2"
    BUNDLE_ID         "com.pulp.magenta.accompanist.v2"
    MANUFACTURER      "PulpMagenta"
    VERSION           "0.1.0"
    CATEGORY          Instrument
    ACCEPTS_MIDI
    PLUGIN_CODE       "PMa2"
    MANUFACTURER_CODE "PuMa"
)

target_link_libraries(PromptableAccompanistV2_Core
    INTERFACE
        magentart::core
        Pulp::audio
        Pulp::view
)

# MLX builds its Metal kernels into mlx.metallib but does not embed that file
# into Pulp bundles automatically. The runtime first looks next to the current
# binary, so copy the metallib beside every V2 bundle executable we generate.
set(PROMPTABLE_ACCOMPANIST_V2_MLX_METALLIB
    "${CMAKE_BINARY_DIR}/_deps/mlx-build/mlx/backend/metal/kernels/mlx.metallib")
find_program(PROMPTABLE_ACCOMPANIST_V2_CODESIGN codesign)

function(promptable_accompanist_v2_copy_mlx_metallib target_name)
    if(NOT TARGET ${target_name})
        return()
    endif()
    if(TARGET mlx-metallib)
        add_dependencies(${target_name} mlx-metallib)
    endif()
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROMPTABLE_ACCOMPANIST_V2_MLX_METALLIB}"
            "$<TARGET_FILE_DIR:${target_name}>/mlx.metallib"
        COMMAND /bin/chmod 0644 "$<TARGET_FILE_DIR:${target_name}>/mlx.metallib"
        COMMENT "Copying MLX Metal kernels for ${target_name}"
        VERBATIM
    )
    if(PROMPTABLE_ACCOMPANIST_V2_CODESIGN)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${PROMPTABLE_ACCOMPANIST_V2_CODESIGN}" --force --deep --sign -
                "$<TARGET_BUNDLE_DIR:${target_name}>"
            COMMENT "Ad-hoc signing ${target_name} after MLX resource copy"
            VERBATIM
        )
    endif()
endfunction()

foreach(target_name
    PromptableAccompanistV2_Standalone
    PromptableAccompanistV2_CLAP
    PromptableAccompanistV2_VST3
    PromptableAccompanistV2_AU
    PromptableAccompanistV2_AUv3
    PromptableAccompanistV2_AUv3Framework
    PromptableAccompanistV2_AUv3Host
)
    promptable_accompanist_v2_copy_mlx_metallib(${target_name})
endforeach()

# V2 is GPU-native only for now. The WebView A/B track stays in E1 while this example
# exercises reusable sampler primitives and the native controls needed for freeze/loop.

if(PULP_BUILD_TESTS)
    add_executable(promptable-accompanist-v2-test test_accompanist.cpp)
    target_link_libraries(promptable-accompanist-v2-test PRIVATE
        Pulp::format
        Pulp::audio
        Pulp::view
        magentart::core
    )
    target_compile_definitions(promptable-accompanist-v2-test PRIVATE
        PROMPTABLE_ACCOMPANIST_V2_TESTING=1
    )
    if(TARGET PromptableAccompanistV2_CLAP)
        add_dependencies(promptable-accompanist-v2-test PromptableAccompanistV2_CLAP)
        target_compile_definitions(promptable-accompanist-v2-test PRIVATE
            PROMPTABLE_ACCOMPANIST_V2_CLAP_BUNDLE_PATH="${CMAKE_BINARY_DIR}/CLAP/PromptableAccompanistV2.clap"
        )
    endif()
    target_include_directories(promptable-accompanist-v2-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME promptable-accompanist-v2-test COMMAND promptable-accompanist-v2-test)
endif()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-PromptableAccompanistV2
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PromptableAccompanistV2.clap/Contents/MacOS/PromptableAccompanistV2'); print('OK')\"")
    set_tests_properties(clap-dlopen-PromptableAccompanistV2 PROPERTIES LABELS "validation;clap" TIMEOUT 20)
endif()

if(PULP_HAS_VST3 AND APPLE)
    add_test(NAME vst3-dlopen-PromptableAccompanistV2
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/VST3/PromptableAccompanistV2.vst3/Contents/MacOS/PromptableAccompanistV2'); print('OK')\"")
    set_tests_properties(vst3-dlopen-PromptableAccompanistV2 PROPERTIES LABELS "validation;vst3" TIMEOUT 20)
endif()
