#include "foo.h"
#include "bar/bar.h"
#include "../runtime/tinycoverage.h"

int main(int argc, char**)
{
    if (argc > 1) {
        foo();
    } else {
        bar();
    }

    tinycoverage::test_finished();

    if (argc == 1) {
        foo();
    } else {
        bar();
    }

    tinycoverage::test_finished();

    tinycoverage::shut_down();
}
