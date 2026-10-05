cmake_minimum_required(VERSION 3.22)

project(Squelch VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")

# Hidden by default, set before JUCE is made available. Two plugins in one host
# process with default visibility can end up sharing JUCE's statics, which
# aborts when one of them unloads.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(FetchContent)

# Declared before the design system so this version wins. It is the same tag
# the design system pins, so the two agree.
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

FetchContent_Declare(
    SpsDesignSystem
    GIT_REPOSITORY https://github.com/slowpulsestudio/sps-juce-design-system.git
    GIT_TAG v1.6.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SpsDesignSystem)

juce_add_plugin(Squelch
    PRODUCT_NAME "Squelch"
    COMPANY_NAME "Slow Pulse Studio"
    BUNDLE_ID "com.slowpulsestudio.squelch"
    PLUGIN_MANUFACTURER_CODE Slps
    PLUGIN_CODE Sqch
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/Slow Pulse Studio"
)

target_sources(Squelch PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(Squelch PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

# The VST3 SDK headers JUCE 9.0.2 ships do not include <memory>, which newer
# Apple Clang no longer pulls in for them. Scoped to C++ because forcing it on
# the Objective-C sources fails outright.
target_compile_options(Squelch PRIVATE
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-include;memory>)

target_link_libraries(Squelch PRIVATE
    sps::DesignSystem
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# The comparison harness. Runs the same DSP the plugin runs and prints its
# results for prototype/compare.py to check against the Python, because
# "it builds" is not evidence that a filter is correct.
add_executable(SquelchHarness Harness/Harness.cpp)
target_compile_features(SquelchHarness PRIVATE cxx_std_17)

# dsp-testing.md's Tests 2, 3, 4, 5 and 15, run against the ported DSP rather
# than the prototype: the comparison harness only ever runs at one sample rate
# in one block configuration, so it cannot see the faults a port introduces.
add_executable(SquelchValidate Harness/Validate.cpp)
target_compile_features(SquelchValidate PRIVATE cxx_std_17)

# Renders the editor to a PNG offscreen. Checking a layout should not require
# a host, a window, or someone willing to go and look at a screen.
juce_add_console_app(SquelchSnapshot PRODUCT_NAME "SquelchSnapshot")
target_sources(SquelchSnapshot PRIVATE Harness/Snapshot.cpp)
target_compile_features(SquelchSnapshot PRIVATE cxx_std_17)
target_compile_definitions(SquelchSnapshot PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
)
target_link_libraries(SquelchSnapshot PRIVATE
    Squelch
    sps::DesignSystem
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
)

# Runs the whole processor, because the engine-level checks cannot see a
# control that never reaches the engines.
juce_add_console_app(SquelchProcessorTest PRODUCT_NAME "SquelchProcessorTest")
target_sources(SquelchProcessorTest PRIVATE Harness/ProcessorTest.cpp)
target_compile_features(SquelchProcessorTest PRIVATE cxx_std_17)
target_compile_definitions(SquelchProcessorTest PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
)
target_link_libraries(SquelchProcessorTest PRIVATE
    Squelch
    sps::DesignSystem
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
)
