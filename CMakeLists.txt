cmake_minimum_required(VERSION 3.18)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_COMPILER clang-13)
set(CMAKE_CXX_COMPILER clang++-13)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

project(tinycoverage)

add_subdirectory(pass)
add_subdirectory(runtime)
add_subdirectory(test)
