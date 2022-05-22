add_library(tinycoverage-runtime tinycoverage_printer.cpp)
set_target_properties(tinycoverage-runtime PROPERTIES 
    COMPILE_FLAGS "-O3" LINK_FLAGS "-flto=thin")
