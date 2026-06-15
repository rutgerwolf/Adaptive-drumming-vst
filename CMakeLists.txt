cmake_minimum_required(VERSION 3.22)
project(AdaptiveDrummer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── JUCE submodule ─────────────────────────────────────────────────────────────
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE submodule not found.\n"
        "Run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/JUCE)

# ── Plugin target ──────────────────────────────────────────────────────────────
juce_add_plugin(AdaptiveDrummer
    PLUGIN_MANUFACTURER_CODE  WnBd
    PLUGIN_CODE               ADrm
    FORMATS                   VST3 Standalone
    PRODUCT_NAME              "Adaptive Drummer"
    COMPANY_NAME              "WinBand Project"
    BUNDLE_ID                 "nl.winband.adaptive-drummer"
    IS_SYNTH                  TRUE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES           "Instrument|Drums"
)

target_sources(AdaptiveDrummer PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/drummer/DrumPattern.cpp
    src/drummer/DrumSampler.cpp
    src/drummer/AdaptiveDrummer.cpp
)

target_include_directories(AdaptiveDrummer PRIVATE src)

target_compile_definitions(AdaptiveDrummer
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AdaptiveDrummer,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AdaptiveDrummer,JUCE_VERSION>"
)

target_link_libraries(AdaptiveDrummer
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(MSVC)
    target_compile_options(AdaptiveDrummer PRIVATE /utf-8)
endif()

juce_generate_juce_header(AdaptiveDrummer)

# Copy assets next to the Standalone binary
add_custom_command(TARGET AdaptiveDrummer_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:AdaptiveDrummer_Standalone>/assets"
    COMMENT "Copying assets to Standalone output"
)

# ── Unit tests ─────────────────────────────────────────────────────────────────
option(ADAPTIVE_DRUMMER_BUILD_TESTS "Build the AdaptiveDrummer unit-test suite" ON)

if(ADAPTIVE_DRUMMER_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(AdaptiveDrummerTests
        PRODUCT_NAME "AdaptiveDrummerTests")

    juce_generate_juce_header(AdaptiveDrummerTests)

    target_sources(AdaptiveDrummerTests PRIVATE
        tests/TestRunner.cpp
        tests/DrumPatternTest.cpp
        tests/DrumSamplerTest.cpp
        src/drummer/DrumPattern.cpp
        src/drummer/DrumSampler.cpp
    )

    target_include_directories(AdaptiveDrummerTests PRIVATE src)

    target_compile_definitions(AdaptiveDrummerTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
    )

    target_link_libraries(AdaptiveDrummerTests
        PRIVATE
            juce::juce_audio_basics
            juce::juce_audio_formats
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )

    if(MSVC)
        target_compile_options(AdaptiveDrummerTests PRIVATE /utf-8)
    endif()

    add_test(NAME AdaptiveDrummerTests COMMAND AdaptiveDrummerTests)
endif()
