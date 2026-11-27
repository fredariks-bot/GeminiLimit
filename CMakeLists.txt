cmake_minimum_required(VERSION 3.22)



project(UltraLimit16X VERSION 1.0.0)



set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)



include(FetchContent)

FetchContent_Declare(

    JUCE

    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git

    GIT_TAG        8.0.4

)

FetchContent_MakeAvailable(JUCE)



juce_add_plugin(UltraLimit16X

    COMPANY_NAME "StudioDSP"

    IS_SYNTH FALSE

    NEEDS_MIDI_INPUT FALSE

    NEEDS_MIDI_OUTPUT FALSE

    IS_MIDI_EFFECT FALSE

    EDITOR_WANTS_KEYBOARD_FOCUS FALSE

    COPY_PLUGIN_AFTER_BUILD TRUE

    PLUGIN_MANUFACTURER_CODE Sdsp

    PLUGIN_CODE Ul16

    FORMATS VST3 AU Standalone

    PRODUCT_NAME "UltraLimit 16X"

)



juce_generate_juce_header(UltraLimit16X)



target_sources(UltraLimit16X

    PRIVATE

        UltraLimit8X.cpp

)



target_compile_definitions(UltraLimit16X

    PUBLIC

        JUCE_WEB_BROWSER=0

        JUCE_USE_CURL=0

        JUCE_VST3_CAN_REPLACE_VST2=0

)



target_link_libraries(UltraLimit16X

    PRIVATE

        juce::juce_audio_utils

        juce::juce_dsp

    PUBLIC

        juce::juce_recommended_config_flags

        juce::juce_recommended_lto_flags

        juce::juce_recommended_warning_flags

)
