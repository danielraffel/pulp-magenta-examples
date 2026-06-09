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
    PLUGIN_CODE       "PMa2"
    MANUFACTURER_CODE "PuMa"
)

target_link_libraries(PromptableAccompanistV2_Core
    INTERFACE
        magentart::core
        Pulp::audio
        Pulp::view
)

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
