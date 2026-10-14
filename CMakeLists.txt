cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Panner VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.10.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(JUCE)

# AAX is built from the SDK bundled with JUCE. The result is unsigned: it loads in
# Pro Tools Developer builds only until it is signed with PACE tools.
set(PANNER_FORMATS VST3 AAX)
if(APPLE)
    list(APPEND PANNER_FORMATS AU)
endif()

juce_add_plugin(Panner
    PRODUCT_NAME "Panner"
    COMPANY_NAME "Anti-Music"
    BUNDLE_ID com.antimusic.panner
    PLUGIN_MANUFACTURER_CODE Antm
    PLUGIN_CODE Pann
    FORMATS ${PANNER_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Spatial
    AAX_CATEGORY SoundField
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Panner PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ProToolsBridge.cpp
    Source/PtslClient.cpp
    Source/DriftPrompt.cpp)

target_compile_definitions(Panner PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Panner
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
