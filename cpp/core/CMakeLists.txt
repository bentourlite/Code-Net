cmake_minimum_required(VERSION 3.22)
project(codenet_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(codenet_core STATIC src/ltc.cpp)
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
endif()
