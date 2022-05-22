#include "bar.h"
#include "../foo.h"
#include <cstdio>

void bar() {
    printf("bar");
	foo();
}
