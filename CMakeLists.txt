cmake_minimum_required(VERSION 3.10)
project(sieve_lab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(sieve_lab_core
  src/wheel_table.cpp
  src/sieve_core.cpp
  src/reference.cpp)
target_include_directories(sieve_lab_core PUBLIC src)

add_executable(bench tools/bench.cpp)
target_link_libraries(bench PRIVATE sieve_lab_core)

enable_testing()
add_executable(check tests/check.cpp)
target_link_libraries(check PRIVATE sieve_lab_core)
add_test(NAME correctness COMMAND check)