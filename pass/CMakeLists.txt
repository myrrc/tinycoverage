find_package(LLVM REQUIRED CONFIG)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

add_library(tinycoverage-pass SHARED pass.cpp)
set_target_properties(tinycoverage-pass PROPERTIES
    COMPILE_FLAGS "-O3 -fno-rtti" LINK_FLAGS "-flto=full")
