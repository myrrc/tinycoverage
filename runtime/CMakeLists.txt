add_library(tinycoverage-runtime tinycoverage.cpp)
set_target_properties(tinycoverage-runtime PROPERTIES COMPILE_FLAGS "-std=c++20 -O3")
