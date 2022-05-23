add_library(tinycoverage-runtime tinycoverage.cpp)
set_target_properties(tinycoverage-runtime PROPERTIES
    COMPILE_FLAGS "-O3" LINK_FLAGS "-flto=full")
