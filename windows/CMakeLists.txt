cmake_minimum_required(VERSION 3.22)
# The patch number is the CI build number, as for the Mac app and plugin.
set(CODENET_BUILD_NUMBER "$ENV{GITHUB_RUN_NUMBER}")
if(NOT CODENET_BUILD_NUMBER)
  set(CODENET_BUILD_NUMBER 0)
endif()
project(CodeNetReceiver VERSION 0.2.${CODENET_BUILD_NUMBER})

set(CMAKE_CXX_STANDARD 17)
if(MSVC)
  # One self-contained .exe: no Visual C++ runtime to install.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

add_subdirectory(../cpp/core ${CMAKE_BINARY_DIR}/codenet_core)

juce_add_gui_app(CodeNetReceiver
  COMPANY_NAME "Tourlite"
  PRODUCT_NAME "Code Net Receiver"
  VERSION ${PROJECT_VERSION})

target_sources(CodeNetReceiver PRIVATE Source/Main.cpp Source/ReceiverEngine.cpp Source/MainComponent.cpp)
target_compile_definitions(CodeNetReceiver PRIVATE
  CODENET_VERSION="${PROJECT_VERSION}"
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(CodeNetReceiver
  PRIVATE codenet_core juce::juce_gui_extra
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
