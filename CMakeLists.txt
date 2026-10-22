cmake_minimum_required(VERSION 3.22)
project(Nagi VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

if(MSVC)
    # 静的ランタイムにしておくと、VC++ランタイム未インストール環境でも読み込める
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

juce_add_plugin(Nagi
    COMPANY_NAME "Aoto"
    PLUGIN_MANUFACTURER_CODE Aoto
    PLUGIN_CODE Nagi
    FORMATS VST3
    PRODUCT_NAME "Nagi"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Dynamics EQ)

target_sources(Nagi PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(Nagi PRIVATE Source)

target_compile_definitions(Nagi PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Nagi
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# オフラインのDSPテスト（CIでは使わない）
option(NAGI_BUILD_TESTS "Build offline DSP test" OFF)
if(NAGI_BUILD_TESTS)
    juce_add_console_app(NagiTest PRODUCT_NAME "NagiTest")
    target_sources(NagiTest PRIVATE tests/OfflineTest.cpp)
    target_include_directories(NagiTest PRIVATE Source)
    target_compile_definitions(NagiTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(NagiTest PRIVATE juce::juce_dsp juce::juce_audio_formats)
endif()
