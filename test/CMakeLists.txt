add_executable(tinycoverage-test main.cpp bar/bar.cpp)
target_link_libraries(tinycoverage-test tinycoverage-runtime)
add_dependencies(tinycoverage-test tinycoverage-pass)

#opt-13 -enable-new-pm=0 -load ./pass/libtinycoverage-pass.so -disable-output
#../hello.ll -goodbye
#
#opt-13 -load-pass-plugin ./pass/libtinycoverage-pass.so
#-passes="goodbye" -disable-output ../hello.ll
#
#clang++-13 -flegacy-pass-manager -Xclang -load -Xclang
#pass/libtinycoverage-pass.so -mllvm -wave-goodbye ../test/main.cpp

set_target_properties(tinycoverage-test PROPERTIES COMPILE_FLAGS
    "-O0 -flegacy-pass-manager \
    -Xclang -load -Xclang ${CMAKE_BINARY_DIR}/pass/libtinycoverage-pass.so \
    -mllvm -wave-goodbye")
