cmake_minimum_required(VERSION 3.22)
project(codenet_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(codenet_core STATIC src/ltc.cpp src/receiver.cpp src/output.cpp src/rx_service.cpp src/c_api.cpp
  src/mac_io.cpp)
find_package(Threads REQUIRED)
target_link_libraries(codenet_core PUBLIC Threads::Threads)
if(APPLE)
  target_link_libraries(codenet_core PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreMIDI"
    "-framework CoreFoundation")
endif()
target_include_directories(codenet_core PUBLIC include)
set_target_properties(codenet_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(codenet_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE codenet_core)
  add_test(NAME core_tests COMMAND core_tests)
  add_executable(receiver_tests tests/receiver_tests.cpp)
  target_link_libraries(receiver_tests PRIVATE codenet_core)
  add_test(NAME receiver_tests COMMAND receiver_tests)
endif()
