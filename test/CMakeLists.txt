add_executable(tinycoverage-test main.cpp bar/bar.cpp)
target_link_libraries(tinycoverage-test tinycoverage-runtime)
add_dependencies(tinycoverage-test tinycoverage-pass)

set_target_properties(tinycoverage-test PROPERTIES COMPILE_FLAGS
    "-O0 -g -flegacy-pass-manager \
    -Xclang -load -Xclang ${CMAKE_BINARY_DIR}/pass/libtinycoverage-pass.so \
    -mllvm -tinycoverage")
