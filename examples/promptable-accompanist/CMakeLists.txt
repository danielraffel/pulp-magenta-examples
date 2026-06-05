# Promptable Accompanist (E1) — MIDI-conditioned generative instrument on magentart::core.
# CLAP-first (auto-fetched + dlopen-testable). Add VST3/AU/Standalone to FORMATS once their
# SDKs are configured — the Processor is format-agnostic.
pulp_add_plugin(PromptableAccompanist
    FORMATS           CLAP
    SOURCES           clap_entry.cpp
    PLUGIN_NAME       "PromptableAccompanist"
    BUNDLE_ID         "com.pulp.magenta.accompanist"
    MANUFACTURER      "PulpMagenta"
    VERSION           "0.1.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PMac"
    MANUFACTURER_CODE "PuMa"
)
# Link the Magenta inference core into the shared processor object library.
target_link_libraries(PromptableAccompanist_Core PUBLIC magentart::core)

if(PULP_BUILD_TESTS)
    add_executable(promptable-accompanist-test test_accompanist.cpp)
    target_link_libraries(promptable-accompanist-test
        PRIVATE pulp::format magentart::core Catch2::Catch2WithMain)
    target_include_directories(promptable-accompanist-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(promptable-accompanist-test)
endif()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-PromptableAccompanist
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PromptableAccompanist.clap/Contents/MacOS/PromptableAccompanist'); print('OK')\"")
    set_tests_properties(clap-dlopen-PromptableAccompanist PROPERTIES LABELS "validation;clap" TIMEOUT 20)
endif()
