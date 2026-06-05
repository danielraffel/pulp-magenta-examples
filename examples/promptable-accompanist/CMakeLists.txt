# Promptable Accompanist (E1) — MIDI-conditioned generative instrument on magentart::core.
# CLAP-first (auto-fetched + dlopen-testable). Add VST3/AU/Standalone to FORMATS once their
# SDKs are configured — the Processor is format-agnostic.
# Header-only processor (accompanist.hpp). Don't list clap_entry.cpp in SOURCES —
# _pulp_add_clap auto-discovers it by convention; listing it too would double-define
# `clap_entry`. With no SOURCES, ${target}_Core is an INTERFACE library.
pulp_add_plugin(PromptableAccompanist
    FORMATS           CLAP VST3 AU AUv3 Standalone
    PLUGIN_NAME       "PromptableAccompanist"
    BUNDLE_ID         "com.pulp.magenta.accompanist"
    MANUFACTURER      "PulpMagenta"
    VERSION           "0.1.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PMac"
    MANUFACTURER_CODE "PuMa"
)
# Propagate the Magenta inference core to the format targets via the INTERFACE core lib.
# Pulp::view is linked for the GPU-native editor (default UI; Skia widgets).
target_link_libraries(PromptableAccompanist_Core INTERFACE magentart::core Pulp::view)

# The GPU-native editor (accompanist_native_ui.hpp) is the default. Opt into the
# alternative WebView editor with -DPROMPTABLE_WEBVIEW_UI=ON (needs a Pulp SDK built
# with -DPULP_BUILD_WEBVIEW=ON). The two are A/B tracks for the same plugin.
option(PROMPTABLE_WEBVIEW_UI "Build the WebView editor UI instead of GPU-native (needs SDK -DPULP_BUILD_WEBVIEW=ON)" OFF)
if(PROMPTABLE_WEBVIEW_UI)
    target_compile_definitions(PromptableAccompanist_Core INTERFACE ACCOMPANIST_WEBVIEW_UI=1)
endif()

if(PULP_BUILD_TESTS)
    add_executable(promptable-accompanist-test test_accompanist.cpp)
    target_link_libraries(promptable-accompanist-test PRIVATE Pulp::format Pulp::view magentart::core)
    if(PROMPTABLE_WEBVIEW_UI)
        target_compile_definitions(promptable-accompanist-test PRIVATE ACCOMPANIST_WEBVIEW_UI=1)
    endif()
    target_include_directories(promptable-accompanist-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME promptable-accompanist-test COMMAND promptable-accompanist-test)
endif()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-PromptableAccompanist
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PromptableAccompanist.clap/Contents/MacOS/PromptableAccompanist'); print('OK')\"")
    set_tests_properties(clap-dlopen-PromptableAccompanist PROPERTIES LABELS "validation;clap" TIMEOUT 20)
endif()

if(PULP_HAS_VST3 AND APPLE)
    add_test(NAME vst3-dlopen-PromptableAccompanist
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/VST3/PromptableAccompanist.vst3/Contents/MacOS/PromptableAccompanist'); print('OK')\"")
    set_tests_properties(vst3-dlopen-PromptableAccompanist PROPERTIES LABELS "validation;vst3" TIMEOUT 20)
endif()
