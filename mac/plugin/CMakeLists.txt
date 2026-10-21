cmake_minimum_required(VERSION 3.22)
project(CodeNetTap VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

add_subdirectory(../../cpp/core ${CMAKE_BINARY_DIR}/codenet_core)

if(APPLE)
  set(CODENET_FORMATS AU VST3)
else()
  set(CODENET_FORMATS VST3)  # for compile checks on Linux CI
endif()

juce_add_plugin(CodeNetTap
  COMPANY_NAME "Tourlite"
  BUNDLE_ID "com.tourlite.codenet.tap"
  PLUGIN_MANUFACTURER_CODE Tlte
  PLUGIN_CODE CnTp
  FORMATS ${CODENET_FORMATS}
  PRODUCT_NAME "Code Net Tap"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(CodeNetTap PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(CodeNetTap PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(CodeNetTap
  PRIVATE codenet_core juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
