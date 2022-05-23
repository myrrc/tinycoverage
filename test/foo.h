#pragma once
#include <cstdio>

inline void inline_func_in_src() {
    printf("Inside inline func in src\n");
}

template <class T>
inline void template_func_in_src() {
    printf("Inside template func in src\n");
}
