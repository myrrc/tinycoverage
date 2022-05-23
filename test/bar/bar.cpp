#include "bar.h"
#include "../foo.h"
#include <cstdio>

void normal_func_in_src() {
    printf("Inside normal func in src , calling inline func in src\n");
	inline_func_in_src();
}
