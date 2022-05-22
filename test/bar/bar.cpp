#include "bar.h"
#include "../foo.h"
#include <cstdio>

void bar() {
    printf("Inside bar, calling foo\n");
	foo();
}
